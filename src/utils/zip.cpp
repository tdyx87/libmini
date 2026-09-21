#include "zip.h"

#include <cstring>
#include <ctime>
#include <map>

#include <zlib.h>

namespace libmini {

namespace {

void put_u16(std::string& out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put_u32(std::string& out, std::uint32_t v)
{
    put_u16(out, static_cast<std::uint16_t>(v & 0xffff));
    put_u16(out, static_cast<std::uint16_t>(v >> 16));
}

std::uint16_t get_u16(const std::string& s, std::size_t off)
{
    if (off + 2 > s.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data()) + off;
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t get_u32(const std::string& s, std::size_t off)
{
    if (off + 4 > s.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data()) + off;
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// DOS 时间戳（zip 规范：本地时间，2 秒精度）
std::uint32_t dos_time_now()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    if (tm.tm_year < 80) {  // 1980 前 zip 无法表达
        return 0x210000;    // 1980-01-01 00:00
    }
    return static_cast<std::uint32_t>(((tm.tm_year + 1900 - 1980) << 25) |
                                      ((tm.tm_mon + 1) << 21) | (tm.tm_mday << 16) |
                                      (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec >> 1));
}

constexpr std::uint16_t kFlagUtf8 = 0x0800;
constexpr std::uint32_t kLocalHeaderSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEocdSig = 0x06054b50;

// zlib deflate → raw deflate 流（无 zlib 头尾）
std::string deflate_raw(const std::string& in)
{
    z_stream zs{};
    if (::deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return "";
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    out.resize(::compressBound(static_cast<uLong>(in.size())) + 64);
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&zs, Z_FINISH);
    ::deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        return "";
    }
    out.resize(out.size() - zs.avail_out);
    return out;
}

// raw inflate（无 zlib 头尾）→ 原始数据；期望长度可先 reserve
bool inflate_raw(const std::string& in, std::uint64_t expected_size, std::string& out)
{
    z_stream zs{};
    if (::inflateInit2(&zs, -15) != Z_OK) {
        return false;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    out.clear();
    out.reserve(static_cast<std::size_t>(expected_size));
    char buf[16 * 1024];
    int rc;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = ::inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            ::inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > (1ULL << 32)) {  // 4GB 炸弹保护
            ::inflateEnd(&zs);
            return false;
        }
    } while (rc != Z_STREAM_END);
    ::inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

}  // namespace

// ---------------- ZipWriter ----------------

struct ZipWriter::Impl
{
    struct Record
    {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t uncompressed_size = 0;
        std::uint32_t local_offset = 0;
        ZipMethod method = ZipMethod::Deflate;
    };

    std::string body;       // 已写出的本地记录区
    std::vector<Record> records;
    bool finished = false;
};

ZipWriter::ZipWriter() : impl_(new Impl) {}
ZipWriter::~ZipWriter() { delete impl_; }

std::size_t ZipWriter::count() const { return impl_->records.size(); }

bool ZipWriter::add_file(const std::string& name, const std::string& data, ZipMethod method)
{
    if (impl_->finished || name.empty() || name.size() > 0xffff) {
        return false;
    }
    const std::uint32_t crc = Crc32::compute(data);

    std::uint32_t method_id = 0;  // store
    std::string stored = data;
    if (method == ZipMethod::Deflate && !data.empty()) {
        std::string deflated = deflate_raw(data);
        if (!deflated.empty() && deflated.size() < data.size()) {
            stored = std::move(deflated);
            method_id = 8;
        }
        // 压缩不赚（<32B 或随机数据）则退回 store
    }

    Impl::Record rec;
    rec.name = name;
    rec.crc = crc;
    rec.uncompressed_size = static_cast<std::uint32_t>(data.size());
    rec.compressed_size = static_cast<std::uint32_t>(stored.size());
    rec.local_offset = static_cast<std::uint32_t>(impl_->body.size());
    rec.method = method_id == 8 ? ZipMethod::Deflate : ZipMethod::Store;

    // 本地文件头
    std::string& b = impl_->body;
    put_u32(b, kLocalHeaderSig);
    put_u16(b, 20);                    // version needed
    put_u16(b, kFlagUtf8);
    put_u16(b, static_cast<std::uint16_t>(method_id));
    put_u32(b, dos_time_now());
    put_u32(b, rec.crc);
    put_u32(b, rec.compressed_size);
    put_u32(b, rec.uncompressed_size);
    put_u16(b, static_cast<std::uint16_t>(name.size()));
    put_u16(b, 0);                     // extra len
    b.append(name);
    b.append(stored);

    impl_->records.push_back(std::move(rec));
    return true;
}

std::string ZipWriter::finish()
{
    if (impl_->finished) {
        return "";
    }
    impl_->finished = true;

    // 中央目录
    const std::uint32_t cd_offset = static_cast<std::uint32_t>(impl_->body.size());
    std::string cd;
    for (const auto& r : impl_->records) {
        put_u32(cd, kCentralSig);
        put_u16(cd, 20);               // version made by
        put_u16(cd, 20);               // version needed
        put_u16(cd, kFlagUtf8);
        put_u16(cd, static_cast<std::uint16_t>(r.method == ZipMethod::Deflate ? 8 : 0));
        put_u32(cd, dos_time_now());
        put_u32(cd, r.crc);
        put_u32(cd, r.compressed_size);
        put_u32(cd, r.uncompressed_size);
        put_u16(cd, static_cast<std::uint16_t>(r.name.size()));
        put_u16(cd, 0);                // extra
        put_u16(cd, 0);                // comment
        put_u16(cd, 0);                // disk number
        put_u16(cd, 0);                // internal attrs
        put_u32(cd, 0);                // external attrs
        put_u32(cd, r.local_offset);
        cd.append(r.name);
    }
    const std::uint32_t cd_size = static_cast<std::uint32_t>(cd.size());

    std::string out = impl_->body;
    out.append(cd);

    // EOCD
    put_u32(out, kEocdSig);
    put_u16(out, 0);                   // disk
    put_u16(out, 0);                   // cd disk
    put_u16(out, static_cast<std::uint16_t>(impl_->records.size()));
    put_u16(out, static_cast<std::uint16_t>(impl_->records.size()));
    put_u32(out, cd_size);
    put_u32(out, cd_offset);
    put_u16(out, 0);                   // comment len
    return out;
}

// ---------------- ZipReader ----------------

ZipReader::ZipReader() = default;
ZipReader::~ZipReader() = default;

bool ZipReader::open(const std::string& zip_bytes)
{
    entries_.clear();
    offsets_.clear();
    error_.clear();

    // 从尾部搜 EOCD（容忍注释 ≤ 64KB）
    if (zip_bytes.size() < 22) {
        error_ = "too small for zip";
        return false;
    }
    std::size_t eocd = std::string::npos;
    const std::size_t search_begin = zip_bytes.size() >= 22 + 65536
                                         ? zip_bytes.size() - 22 - 65536
                                         : 0;
    for (std::size_t i = zip_bytes.size() - 22 + 1; i-- > search_begin;) {
        if (get_u32(zip_bytes, i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        error_ = "EOCD not found";
        return false;
    }
    const std::uint16_t count = get_u16(zip_bytes, eocd + 10);
    const std::uint32_t cd_size = get_u32(zip_bytes, eocd + 12);
    const std::uint32_t cd_offset = get_u32(zip_bytes, eocd + 16);
    if (cd_offset + cd_size > zip_bytes.size()) {
        error_ = "central directory out of range";
        return false;
    }

    data_ = zip_bytes;
    std::size_t p = cd_offset;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (p + 46 > zip_bytes.size() || get_u32(zip_bytes, p) != kCentralSig) {
            error_ = "bad central directory";
            entries_.clear();
            return false;
        }
        ZipEntryInfo e;
        const std::uint16_t method_id = get_u16(zip_bytes, p + 10);
        e.crc32 = get_u32(zip_bytes, p + 16);
        e.compressed_size = get_u32(zip_bytes, p + 20);
        e.uncompressed_size = get_u32(zip_bytes, p + 24);
        const std::uint16_t name_len = get_u16(zip_bytes, p + 28);
        const std::uint16_t extra_len = get_u16(zip_bytes, p + 30);
        const std::uint16_t comment_len = get_u16(zip_bytes, p + 32);
        const std::uint32_t local_off = get_u32(zip_bytes, p + 42);
        e.name = zip_bytes.substr(p + 46, name_len);
        e.method = method_id == 8 ? ZipMethod::Deflate : ZipMethod::Store;
        entries_.push_back(e);

        // 定位数据起点：本地头 30B + name + extra（extra 长度可能中央/本地不一致，须读本地头）
        if (local_off + 30 > zip_bytes.size() || get_u32(zip_bytes, local_off) != kLocalHeaderSig) {
            error_ = "bad local header";
            entries_.clear();
            return false;
        }
        const std::uint16_t lname = get_u16(zip_bytes, local_off + 26);
        const std::uint16_t lextra = get_u16(zip_bytes, local_off + 28);
        offsets_.emplace_back(local_off, local_off + 30 + lname + lextra);

        p += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

const std::vector<ZipEntryInfo>& ZipReader::entries() const { return entries_; }

bool ZipReader::contains(const std::string& name) const
{
    for (const auto& e : entries_) {
        if (e.name == name) {
            return true;
        }
    }
    return false;
}

std::string ZipReader::extract(const std::string& name)
{
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            return extract_at(i);
        }
    }
    error_ = "entry not found: " + name;
    return "";
}

std::string ZipReader::extract_at(std::size_t index)
{
    error_.clear();
    if (index >= entries_.size()) {
        error_ = "bad index";
        return "";
    }
    const ZipEntryInfo& e = entries_[index];
    const std::size_t data_off = offsets_[index].second;
    const std::string stored = data_.substr(data_off,
        static_cast<std::size_t>(e.compressed_size));

    std::string raw;
    if (e.method == ZipMethod::Store) {
        raw = stored;
    } else {
        if (!inflate_raw(stored, e.uncompressed_size, raw)) {
            error_ = "inflate failed";
            return "";
        }
    }
    if (raw.size() != e.uncompressed_size) {
        error_ = "size mismatch";
        return "";
    }
    if (Crc32::compute(raw) != e.crc32) {
        error_ = "crc mismatch";
        return "";
    }
    return raw;
}

}  // namespace libmini
