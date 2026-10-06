#include "tar.h"

#include <cstdio>
#include <cstring>

namespace libmini {

namespace {

constexpr std::size_t kBlockSize = 512;

// 头部字段偏移（POSIX ustar）
constexpr std::size_t kOffName = 0;      // 100
constexpr std::size_t kOffMode = 100;    // 8
constexpr std::size_t kOffUid = 108;     // 8
constexpr std::size_t kOffGid = 116;     // 8
constexpr std::size_t kOffSize = 124;    // 12
constexpr std::size_t kOffMtime = 136;   // 12
constexpr std::size_t kOffChksum = 148;  // 8
constexpr std::size_t kOffTypeflag = 156;
constexpr std::size_t kOffLinkname = 157;   // 100
constexpr std::size_t kOffMagic = 257;      // 6 "ustar\0"
constexpr std::size_t kOffVersion = 263;    // 2 "00"
constexpr std::size_t kOffUname = 265;      // 32
constexpr std::size_t kOffGname = 297;      // 32
constexpr std::size_t kOffDevmajor = 329;   // 8
constexpr std::size_t kOffDevminor = 337;   // 8
constexpr std::size_t kOffPrefix = 345;     // 155

// 定长字段写入：拷贝字节，剩余保持 0（NUL 结尾的 C 串语义）
void put_bytes(char* hdr, std::size_t off, const char* data, std::size_t len)
{
    std::memcpy(hdr + off, data, len);
}

// 八进制数值字段写入（octal + NUL 结尾）；数值超宽时按最大值截断写入
void put_octal(char* hdr, std::size_t off, std::size_t width,
               std::uint64_t value)
{
    // 宽度为 width 的八进制串占 width-1 位 + 1 位 NUL
    char buf[32];
    std::size_t n = 0;
    std::uint64_t v = value;
    do {
        buf[n++] = static_cast<char>('0' + (v & 7));
        v >>= 3;
    } while (v != 0 && n < sizeof(buf));
    if (n > width - 1) {
        n = width - 1;  // 超宽：截断（调用方已保证正常值不溢出）
    }
    char* p = hdr + off;
    for (std::size_t i = 0; i < width; ++i) {
        p[i] = '0';
    }
    for (std::size_t i = 0; i < n; ++i) {
        p[width - 2 - i] = buf[i];
    }
    p[width - 1] = '\0';
}

// 解析八进制数值字段（容忍尾随 NUL/空格；遇非八进制字符即停）
bool get_octal(const std::string& s, std::size_t off, std::size_t width,
               std::uint64_t& out)
{
    out = 0;
    bool any = false;
    for (std::size_t i = 0; i < width; ++i) {
        const char c = s[off + i];
        if (c == '\0' || c == ' ') {
            if (any) break;
            continue;
        }
        if (c < '0' || c > '7') {
            return false;
        }
        out = (out << 3) + static_cast<std::uint64_t>(c - '0');
        any = true;
    }
    return true;
}

// 读定长字符串字段（到 NUL 或字段末尾）
std::string get_str(const std::string& s, std::size_t off, std::size_t width)
{
    std::size_t len = 0;
    while (len < width && s[off + len] != '\0') {
        ++len;
    }
    return s.substr(off, len);
}

// 计算头部 checksum：chksum 字段本身按空格参与求和
std::uint32_t compute_checksum(const char* hdr)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        if (i >= kOffChksum && i < kOffChksum + 8) {
            sum += ' ';
        } else {
            sum += static_cast<unsigned char>(hdr[i]);
        }
    }
    return sum;
}

// 全零块：tar 的 EOF 与填充标志
bool is_zero_block(const char* p)
{
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        if (p[i] != '\0') return false;
    }
    return true;
}

std::uint64_t round_up_512(std::uint64_t n)
{
    return (n + kBlockSize - 1) / kBlockSize * kBlockSize;
}

}  // namespace

// ------------------------------ TarWriter ------------------------------

TarWriter::TarWriter() = default;

std::size_t TarWriter::count() const { return count_; }

bool TarWriter::add_header(const std::string& name,
                           const std::string& link, TarType type,
                           std::uint64_t size, std::uint32_t mode)
{
    if (name.empty()) {
        error_ = "tar: 条目名为空";
        return false;
    }
    if (name.size() > kOffPrefix + 1 + 100) {  // prefix(155) + '/' + name(100)
        error_ = "tar: 条目名过长（>255 字节）: " + name;
        return false;
    }
    if (type == TarType::Symlink && link.size() > 100) {
        error_ = "tar: 链接目标过长（>100 字节）: " + link;
        return false;
    }

    char hdr[kBlockSize];
    std::memset(hdr, 0, sizeof(hdr));

    // 名字拆分：>100 字节时把前缀放 prefix 字段（在 '/' 处断开）
    std::string head = name;
    std::string prefix;
    if (name.size() > 100) {
        // 合法分割点 split 需同时满足：
        //   head = name.substr(split+1) <= 100 字节 → split >= n-101
        //   prefix = name.substr(0, split) <= 155 字节 → split <= 155
        //   且 name[split] == '/'、prefix/head 都非空
        const std::size_t n = name.size();
        const std::size_t lo = n > 101 ? n - 101 : 1;  // 需 prefix 非空
        const std::size_t hi = n - 2 < 155 ? n - 2 : 155;  // 需 head 非空
        std::size_t split = std::string::npos;
        for (std::size_t i = lo; i <= hi; ++i) {
            if (name[i] == '/') {
                split = i;
                break;
            }
        }
        if (split == std::string::npos) {
            error_ = "tar: 无法拆分过长条目名（范围内无 '/'）: " + name;
            return false;
        }
        prefix = name.substr(0, split);
        head = name.substr(split + 1);
    }
    if (head.size() > 100 || prefix.size() > 155) {
        error_ = "tar: 条目名过长: " + name;
        return false;
    }

    put_bytes(hdr, kOffName, head.data(), head.size());
    put_octal(hdr, kOffMode, 8, mode & 07777);
    put_octal(hdr, kOffUid, 8, 0);
    put_octal(hdr, kOffGid, 8, 0);
    put_octal(hdr, kOffSize, 12, size);
    put_octal(hdr, kOffMtime, 12, 0);
    // chksum 先按空格填充（compute_checksum 也按空格算，保持一致）
    std::memset(hdr + kOffChksum, ' ', 8);
    hdr[kOffTypeflag] = static_cast<char>(type);
    if (!link.empty()) {
        put_bytes(hdr, kOffLinkname, link.data(), link.size());
    }
    put_bytes(hdr, kOffMagic, "ustar", 5);  // 第 6 字节保持 \0
    put_bytes(hdr, kOffVersion, "00", 2);
    put_octal(hdr, kOffDevmajor, 8, 0);
    put_octal(hdr, kOffDevminor, 8, 0);
    if (!prefix.empty()) {
        put_bytes(hdr, kOffPrefix, prefix.data(), prefix.size());
    }

    // checksum：6 位八进制 + NUL + 空格
    const std::uint32_t sum = compute_checksum(hdr);
    char cbuf[8];
    std::snprintf(cbuf, sizeof(cbuf), "%06o ", sum);
    std::memcpy(hdr + kOffChksum, cbuf, 7);
    hdr[kOffChksum + 7] = '\0';

    data_.append(hdr, kBlockSize);
    return true;
}

bool TarWriter::add_file(const std::string& name, const std::string& data,
                         std::uint32_t mode)
{
    if (!add_header(name, std::string(), TarType::Regular, data.size(), mode)) {
        return false;
    }
    data_ += data;
    // 数据按 512 字节补齐
    const std::size_t pad = static_cast<std::size_t>(
        round_up_512(data.size()) - data.size());
    if (pad > 0) {
        data_.append(pad, '\0');
    }
    ++count_;
    return true;
}

bool TarWriter::add_dir(const std::string& name, std::uint32_t mode)
{
    std::string dir = name;
    if (!dir.empty() && dir.back() != '/') {
        dir += '/';
    }
    if (!add_header(dir, std::string(), TarType::Directory, 0, mode)) {
        return false;
    }
    ++count_;
    return true;
}

bool TarWriter::add_symlink(const std::string& name, const std::string& target,
                            std::uint32_t mode)
{
    if (!add_header(name, target, TarType::Symlink, 0, mode)) {
        return false;
    }
    ++count_;
    return true;
}

std::string TarWriter::finish()
{
    // 两个全零块收尾（1024 字节 EOF 标记）
    data_.append(kBlockSize * 2, '\0');
    std::string out;
    out.swap(data_);
    data_.clear();
    count_ = 0;
    return out;
}

// ------------------------------ TarReader ------------------------------

TarReader::TarReader() = default;

bool TarReader::open(const std::string& tar_bytes)
{
    entries_.clear();
    data_spans_.clear();
    error_.clear();
    data_ = tar_bytes;

    if (tar_bytes.size() < kBlockSize * 2) {
        error_ = "tar: 数据过短，不是有效 tar";
        return false;
    }
    if (tar_bytes.size() % kBlockSize != 0) {
        error_ = "tar: 数据长度不是 512 的倍数";
        return false;
    }

    std::size_t pos = 0;
    bool saw_eof = false;
    while (pos + kBlockSize <= tar_bytes.size()) {
        const char* hdr = tar_bytes.data() + pos;
        if (is_zero_block(hdr)) {
            saw_eof = true;
            pos += kBlockSize;
            continue;  // 允许 EOF 后仍有零块/填充
        }
        if (saw_eof) {
            error_ = "tar: EOF 标记后仍有数据块";
            return false;
        }

        // 校验 magic
        if (std::memcmp(hdr + kOffMagic, "ustar", 5) != 0) {
            error_ = "tar: 缺少 ustar magic（偏移 " + std::to_string(pos) + "）";
            return false;
        }
        // 校验 checksum：字段存的是 6 位八进制 + NUL + 空格
        std::uint64_t stored = 0;
        if (!get_octal(tar_bytes, pos + kOffChksum, 8, stored)) {
            error_ = "tar: checksum 字段非法（偏移 " + std::to_string(pos) + "）";
            return false;
        }
        if (stored != compute_checksum(hdr)) {
            error_ = "tar: checksum 不匹配（偏移 " + std::to_string(pos) + "）";
            return false;
        }

        std::uint64_t size = 0;
        if (!get_octal(tar_bytes, pos + kOffSize, 12, size)) {
            error_ = "tar: size 字段非法（偏移 " + std::to_string(pos) + "）";
            return false;
        }
        std::uint64_t mode = 0;
        get_octal(tar_bytes, pos + kOffMode, 8, mode);  // mode 宽松解析
        std::uint64_t mtime = 0;
        get_octal(tar_bytes, pos + kOffMtime, 12, mtime);

        TarEntryInfo e;
        const std::string fname = get_str(tar_bytes, pos + kOffName, 100);
        const std::string fprefix = get_str(tar_bytes, pos + kOffPrefix, 155);
        e.name = fprefix.empty() ? fname : fprefix + "/" + fname;
        e.size = size;
        e.mode = static_cast<std::uint32_t>(mode);
        e.mtime = static_cast<std::int64_t>(mtime);
        const char tf = hdr[kOffTypeflag];
        if (tf == '\0' || tf == '0') {
            e.type = TarType::Regular;
        } else if (tf == '1') {
            e.type = TarType::HardLink;
        } else if (tf == '2') {
            e.type = TarType::Symlink;
        } else if (tf == '5') {
            e.type = TarType::Directory;
        } else {
            error_ = "tar: 不支持的 typeflag '" + std::string(1, tf) +
                     "'（偏移 " + std::to_string(pos) + "）";
            return false;
        }
        if (e.type == TarType::Symlink || e.type == TarType::HardLink) {
            e.link_target = get_str(tar_bytes, pos + kOffLinkname, 100);
        }
        if (e.name.empty()) {
            error_ = "tar: 条目名为空（偏移 " + std::to_string(pos) + "）";
            return false;
        }

        const std::size_t data_off = pos + kBlockSize;
        const std::uint64_t padded = round_up_512(size);
        if (data_off + padded > tar_bytes.size()) {
            error_ = "tar: 条目数据被截断: " + e.name;
            return false;
        }

        entries_.push_back(e);
        data_spans_.push_back(
            std::make_pair(data_off, static_cast<std::size_t>(size)));
        pos = data_off + static_cast<std::size_t>(padded);
    }

    if (entries_.empty()) {
        error_ = "tar: 没有任何条目";
        return false;
    }
    return true;
}

const std::vector<TarEntryInfo>& TarReader::entries() const { return entries_; }

bool TarReader::contains(const std::string& name) const
{
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) return true;
    }
    return false;
}

std::string TarReader::extract(const std::string& name)
{
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            return extract_at(i);
        }
    }
    error_ = "tar: 找不到条目: " + name;
    return std::string();
}

std::string TarReader::extract_at(std::size_t index)
{
    if (index >= entries_.size()) {
        error_ = "tar: 序号越界: " + std::to_string(index);
        return std::string();
    }
    const TarEntryInfo& e = entries_[index];
    if (e.type == TarType::Directory || e.type == TarType::Symlink ||
        e.type == TarType::HardLink) {
        return std::string();  // 无数据载荷
    }
    // 数据偏移指向 open() 时保存的原始字节
    const std::pair<std::size_t, std::size_t>& span = data_spans_[index];
    if (span.first + span.second > data_.size()) {
        error_ = "tar: 条目数据越界: " + e.name;
        return std::string();
    }
    return data_.substr(span.first, span.second);
}

}  // namespace libmini
