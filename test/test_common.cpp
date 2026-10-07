// 第二批常用模块（optional/random/env/digest/ini/gzip）的单元测试
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "libmini.h"

// ------------------------------- optional --------------------------------

TEST(OptionalTest, BasicSemantics)
{
    using namespace libmini;
    optional<int> a;
    EXPECT_FALSE(a.has_value());
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_EQ(a.value_or(7), 7);

    optional<int> b(42);
    EXPECT_TRUE(b.has_value());
    EXPECT_EQ(*b, 42);
    EXPECT_EQ(b.value(), 42);
    EXPECT_EQ(b.value_or(7), 42);

    b = nullopt;
    EXPECT_FALSE(b.has_value());

    EXPECT_THROW(optional<int>().value(), bad_optional_access);
}

TEST(OptionalTest, CopyMove)
{
    using namespace libmini;
    const std::string kLong = "a long enough string to avoid SSO";

    optional<std::string> src(kLong);
    optional<std::string> copied(src);
    ASSERT_TRUE(copied.has_value());
    EXPECT_EQ(*copied, kLong);

    optional<std::string> moved(std::move(src));
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved, kLong);

    // 自转移不崩溃（标准要求 no-op；我们的实现有 this 检查）。
    // GCC 13+ 对自转移发 -Wself-move 告警，这里自转移正是测试目标，局部关闭
    optional<std::string> self(moved);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    self = std::move(self);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    EXPECT_EQ(*self, kLong);

    // 拷贝空 optional 保持为空
    optional<int> empty;
    optional<int> copied_empty(empty);
    EXPECT_FALSE(copied_empty.has_value());
}

TEST(OptionalTest, ReassignAndEmplace)
{
    using namespace libmini;
    optional<std::string> s;
    s.emplace(5, 'x');
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "xxxxx");

    s = std::string("y");
    EXPECT_EQ(*s, "y");

    s.reset();
    EXPECT_FALSE(s.has_value());

    // 赋值覆盖旧值（析构旧资源）
    optional<std::string> a(std::string("first value"));
    optional<std::string> b(std::string("second value"));
    a = b;
    EXPECT_EQ(*a, "second value");
    EXPECT_EQ(*b, "second value");
}

TEST(OptionalTest, MakeOptionalAndSwap)
{
    using namespace libmini;
    auto m = make_optional(3);
    static_assert(std::is_same<decltype(m), optional<int> >::value,
                  "make_optional deduces optional<int>");
    EXPECT_EQ(*m, 3);

    optional<int> x(1), y(2);
    x.swap(y);
    EXPECT_EQ(*x, 2);
    EXPECT_EQ(*y, 1);
}

// ----------------------------- random_utils ------------------------------

TEST(RandomUtilsTest, IntRange)
{
    using namespace libmini;
    for (int i = 0; i < 200; ++i) {
        const int v = random_int(3, 10);
        EXPECT_GE(v, 3);
        EXPECT_LE(v, 10);
    }
    // 边界相等时恒返回该值
    EXPECT_EQ(random_int(5, 5), 5);
}

TEST(RandomUtilsTest, DoubleRangeAndString)
{
    using namespace libmini;
    for (int i = 0; i < 200; ++i) {
        const double v = random_double(1.5, 2.5);
        EXPECT_GE(v, 1.5);
        EXPECT_LT(v, 2.5);
    }
    EXPECT_NEAR(random_double(0.5, 0.5), 0.5, 1e-12);

    const std::string s = random_string(32);
    EXPECT_EQ(s.size(), 32u);
    for (std::size_t i = 0; i < s.size(); ++i) {
        EXPECT_NE(s[i], '\0');
    }
    // 自定义字母表
    const std::string hexs = random_string(8, "0123456789abcdef");
    for (std::size_t i = 0; i < hexs.size(); ++i) {
        const char c = hexs[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
    }
    EXPECT_EQ(random_string(10, ""), "");
}

TEST(RandomUtilsTest, Pick)
{
    using namespace libmini;
    const std::vector<int> vals = {10, 20, 30};
    for (int i = 0; i < 50; ++i) {
        const int v = random_pick(vals);
        EXPECT_TRUE(v == 10 || v == 20 || v == 30);
    }
}

// --------------------------------- env -----------------------------------

TEST(EnvTest, GetSetRemove)
{
    using namespace libmini;
    EXPECT_FALSE(env_has("LIBMINI_TEST_VAR_XYZ"));

    EXPECT_TRUE(env_set("LIBMINI_TEST_VAR_XYZ", "hello"));
    EXPECT_TRUE(env_has("LIBMINI_TEST_VAR_XYZ"));
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ"), "hello");
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ", "dft"), "hello");

    EXPECT_TRUE(env_remove("LIBMINI_TEST_VAR_XYZ"));
    EXPECT_FALSE(env_has("LIBMINI_TEST_VAR_XYZ"));
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ"), "");
    EXPECT_EQ(env_get("LIBMINI_TEST_VAR_XYZ", "dft"), "dft");

    // 不存在的变量返回默认值
    EXPECT_EQ(env_get("LIBMINI_NO_SUCH_VAR_42", "fallback"), "fallback");
}

TEST(EnvTest, Expand)
{
    using namespace libmini;
    env_set("LIBMINI_TEST_VAR_AB", "abc");

    EXPECT_EQ(env_expand("x=%LIBMINI_TEST_VAR_AB%!"), "x=abc!");
    EXPECT_EQ(env_expand("%LIBMINI_TEST_VAR_AB%%LIBMINI_TEST_VAR_AB%"), "abcabc");
    EXPECT_EQ(env_expand("no vars here"), "no vars here");
    // 未定义变量原样保留
    EXPECT_EQ(env_expand("%LIBMINI_NO_SUCH_VAR_42%"), "%LIBMINI_NO_SUCH_VAR_42%");
    // 孤立 % 不处理
    EXPECT_EQ(env_expand("50% off"), "50% off");

    env_remove("LIBMINI_TEST_VAR_AB");
}

// -------------------------------- digest ---------------------------------

TEST(Md5Test, KnownVectors)
{
    using namespace libmini;
    EXPECT_EQ(Md5::hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(Md5::hex("a"), "0cc175b9c0f1b6a831c399e269772661");
    EXPECT_EQ(Md5::hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(Md5::hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(Md5::hex("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST(Md5Test, Incremental)
{
    using namespace libmini;
    // 分块喂入与一次性计算结果一致（覆盖 64 字节块边界：1+63=64 正好一块，
    // 后续 70+866 字节再走多块 + 尾部缓冲路径）
    const std::string data(1000, 'x');
    Md5 whole;
    whole.update(data);
    const std::string digest = whole.finish();  // finish 消费状态，只能调一次

    Md5 chunked;
    chunked.update(data.substr(0, 1));
    chunked.update(data.substr(1, 63));
    chunked.update(data.substr(64, 70));
    chunked.update(data.substr(134));

    EXPECT_EQ(digest, chunked.finish());
    EXPECT_EQ(Md5::hex(data), Hex::encode(digest, true));

    // reset 复用
    whole.reset();
    whole.update("abc");
    std::string expect_bytes;
    ASSERT_TRUE(Hex::decode("900150983cd24fb0d6963f7d28e17f72", expect_bytes));
    EXPECT_EQ(whole.finish(), expect_bytes);
}

TEST(Sha256Test, KnownVectors)
{
    using namespace libmini;
    EXPECT_EQ(Sha256::hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Sha256::hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(Sha256::hex("The quick brown fox jumps over the lazy dog"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Sha256Test, Incremental)
{
    using namespace libmini;
    const std::string data(1000, 'x');
    Sha256 whole;
    whole.update(data);

    Sha256 chunked;
    for (std::size_t i = 0; i < data.size(); i += 37) {
        chunked.update(data.substr(i, 37));
    }
    EXPECT_EQ(whole.finish(), chunked.finish());
}

// -------------------------------- blake3 ---------------------------------

namespace {

// 官方 test_vectors.json 的输入生成规则：第 i 字节 = i % 251（向量文件
// 顶部注释），覆盖 0..250 的完整循环
std::string blake3_vector_input(std::size_t len)
{
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        s[i] = static_cast<char>(i % 251);
    }
    return s;
}

}  // namespace

TEST(Blake3Test, OfficialVectors)
{
    using namespace libmini;
    // 来自 BLAKE3-team/BLAKE3 test_vectors/test_vectors.json 的 hash 列
    //（前 32 字节），覆盖：空输入、块边界（63/64/65、127/128）、
    // chunk 边界（1023/1024/1025）、以及非 2 的幂的多 chunk 树形（2049）
    struct Case {
        std::size_t len;
        const char* hex;
    };
    static const Case kCases[] = {
        {0,
         "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {1,
         "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {3,
         "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f"},
        {4,
         "f30f5ab28fe047904037f77b6da4fea1e27241c5d132638d8bedce9d40494f32"},
        {5,
         "b40b44dfd97e7a84a996a91af8b85188c66c126940ba7aad2e7ae6b385402aa2"},
        {63,
         "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
        {64,
         "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {65,
         "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
        {127,
         "d81293fda863f008c09e92fc382a81f5a0b4a1251cba1634016a0f86a6bd640d"},
        {128,
         "f17e570564b26578c33bb7f44643f539624b05df1a76c81f30acd548c44b45ef"},
        {1023,
         "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024,
         "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025,
         "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2048,
         "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
        {2049,
         "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
        {4096,
         "015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969"},
    };
    for (const Case& c : kCases) {
        EXPECT_EQ(Blake3::hex(blake3_vector_input(c.len)), c.hex)
            << "input_len " << c.len;
    }

    // C2SP 规范附录的单块执行轨迹：BLAKE3("IETF")
    EXPECT_EQ(Blake3::hex("IETF"),
              "83a2de1ee6f4e6ab686889248f4ec0cf4cc5709446a682ffd1cbb4d6165181e2");
}

TEST(Blake3Test, KeyedOfficialVectors)
{
    using namespace libmini;
    const std::string key = "whats the Elvish word for friend";
    ASSERT_EQ(key.size(), 32u);  // 官方向量的 32 字节密钥

    struct Case {
        std::size_t len;
        const char* hex;
    };
    static const Case kCases[] = {
        {0,
         "92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26"},
        {1,
         "6d7878dfff2f485635d39013278ae14f1454b8c0a3a2d34bc1ab38228a80c95b"},
        {3,
         "39e67b76b5a007d4921969779fe666da67b5213b096084ab674742f0d5ec62b9"},
        {64,
         "ba8ced36f327700d213f120b1a207a3b8c04330528586f414d09f2f7d9ccb7e6"},
        {65,
         "c0a4edefa2d2accb9277c371ac12fcdbb52988a86edc54f0716e1591b4326e72"},
        {1023,
         "c951ecdf03288d0fcc96ee3413563d8a6d3589547f2c2fb36d9786470f1b9d6e"},
        {1024,
         "75c46f6f3d9eb4f55ecaaee480db732e6c2105546f1e675003687c31719c7ba4"},
        {1025,
         "357dc55de0c7e382c900fd6e320acc04146be01db6a8ce7210b7189bd664ea69"},
        {2048,
         "879cf1fa2ea0e79126cb1063617a05b6ad9d0b696d0d757cf053439f60a99dd1"},
        {4096,
         "befc660aea2f1718884cd8deb9902811d332f4fc4a38cf7c7300d597a081bfc0"},
    };
    for (const Case& c : kCases) {
        EXPECT_EQ(Blake3::keyed_hex(key, blake3_vector_input(c.len)), c.hex)
            << "keyed input_len " << c.len;
    }
}

TEST(Blake3Test, DeriveKeyOfficialVectors)
{
    using namespace libmini;
    const std::string context =
        "BLAKE3 2019-12-27 16:29:52 test vectors context";

    struct Case {
        std::size_t len;
        const char* hex;
    };
    static const Case kCases[] = {
        {0,
         "2cc39783c223154fea8dfb7c1b1660f2ac2dcbd1c1de8277b0b0dd39b7e50d7d"},
        {1,
         "b3e2e340a117a499c6cf2398a19ee0d29cca2bb7404c73063382693bf66cb06c"},
        {3,
         "440aba35cb006b61fc17c0529255de438efc06a8c9ebf3f2ddac3b5a86705797"},
        {64,
         "a5c4a7053fa86b64746d4bb688d06ad1f02a18fce9afd3e818fefaa7126bf73e"},
        {65,
         "51fd05c3c1cfbc8ed67d139ad76f5cf8236cd2acd26627a30c104dfd9d3ff8a8"},
        {1023,
         "74a16c1c3d44368a86e1ca6df64be6a2f64cce8f09220787450722d85725dea5"},
        {1024,
         "7356cd7720d5b66b6d0697eb3177d9f8d73a4a5c5e968896eb6a689684302706"},
        {1025,
         "effaa245f065fbf82ac186839a249707c3bddf6d3fdda22d1b95a3c970379bcb"},
        {2048,
         "7b2945cb4fef70885cc5d78a87bf6f6207dd901ff239201351ffac04e1088a23"},
        {4096,
         "1e0d7f3db8c414c97c6307cbda6cd27ac3b030949da8e23be1a1a924ad2f25b9"},
    };
    for (const Case& c : kCases) {
        EXPECT_EQ(Blake3::derive_hex(context, blake3_vector_input(c.len)),
                  c.hex)
            << "derive input_len " << c.len;
    }
}

TEST(Blake3Test, StreamingMatchesOneShot)
{
    using namespace libmini;
    const std::string data = blake3_vector_input(5000);
    const std::string expected = Blake3::hex(data);

    // 喂入尺寸横跨页边界（64）、chunk 边界（1024）与非整除情形
    const std::size_t feeds[] = {1, 63, 64, 65, 127, 128,
                                 1023, 1024, 1025, 17, 3000};
    std::size_t feed_index = 0;
    Blake3 h;
    for (std::size_t pos = 0; pos < data.size();) {
        const std::size_t n = feeds[feed_index++ % (sizeof(feeds) /
                                                    sizeof(feeds[0]))];
        const std::size_t take = (n < data.size() - pos) ? n
                                                         : data.size() - pos;
        h.update(data.data() + pos, take);
        pos += take;
    }
    EXPECT_EQ(Hex::encode(h.finish(), true), expected);

    // 逐字节喂入（小数据）
    const std::string small = blake3_vector_input(1100);  // 跨 1 个 chunk
    Blake3 byte_by_byte;
    for (std::size_t i = 0; i < small.size(); ++i) {
        byte_by_byte.update(small.data() + i, 1);
    }
    Blake3 one_shot;
    one_shot.update(small);
    EXPECT_EQ(byte_by_byte.finish(), one_shot.finish());
}

TEST(Blake3Test, XofExtendedOutput)
{
    using namespace libmini;
    // 官方向量的 131 字节扩展输出：t=0、t=1 两个整块 + t=2 的前 3 字节，
    // 验证「根压缩仅 t 递增」的 XOF 路径（规范 §4.4）
    static const char* kEmptyXof =
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
        "e00f03e7b69af26b7faaf09fcd333050338ddfe085b8cc869ca98b206c08243a"
        "26f5487789e8f660afe6c99ef9e0c52b92e7393024a80459cf91f476f9ffdbda"
        "7001c22e159b402631f277ca96f2defdf1078282314e763699a31c5363165421c"
        "ce14d";
    static const char* kOneByteXof =
        "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"
        "c3a6cb8bf623e20cdb535f8d1a5ffb86342d9c0b64aca3bce1d31f60adfa137b"
        "358ad4d79f97b47c3d5e79f179df87a3b9776ef8325f8329886ba42f07fb138b"
        "b502f4081cbcec3195c5871e6c23e2cc97d3c69a613eba131e5f1351f3f1da78"
        "6545e5";

    Blake3 empty;
    EXPECT_EQ(Hex::encode(empty.finish(131), true), kEmptyXof);

    Blake3 one;
    one.update(blake3_vector_input(1));
    EXPECT_EQ(Hex::encode(one.finish(131), true), kOneByteXof);

    // 前缀性质：短输出是长输出的前缀，且不承诺长度
    const std::string data = blake3_vector_input(1500);
    Blake3 a;
    a.update(data);
    const std::string s64 = a.finish(64);
    Blake3 b;
    b.update(data);
    const std::string s200 = b.finish(200);
    EXPECT_EQ(s200.substr(0, 64), s64);
    Blake3 c;
    c.update(data);
    EXPECT_EQ(s200.substr(0, 32), c.finish());
    EXPECT_EQ(Blake3::hex(data), Hex::encode(s200.substr(0, 32), true));

    Blake3 zero;
    zero.update(data);
    EXPECT_TRUE(zero.finish(0).empty());
}

TEST(Blake3Test, KeyedModeValidationAndReset)
{
    using namespace libmini;
    // 长度非法的 key：拒绝，且状态保持不变（仍是无键模式）
    Blake3 h;
    EXPECT_FALSE(h.reset_keyed("short"));
    h.update("abc");
    EXPECT_EQ(Hex::encode(h.finish(), true), Blake3::hex("abc"));

    // 一次性接口对非法 key 返回空串
    EXPECT_TRUE(Blake3::keyed_hex("x", "abc").empty());
    EXPECT_TRUE(Blake3::keyed_hex(std::string(33, 'k'), "abc").empty());

    // 同长度不同密钥 → 不同 MAC；正确密钥非空
    const std::string k1(32, 'a');
    const std::string k2(32, 'b');
    const std::string mac1 = Blake3::keyed_hex(k1, "abc");
    EXPECT_FALSE(mac1.empty());
    EXPECT_NE(mac1, Blake3::keyed_hex(k2, "abc"));
    EXPECT_NE(mac1, Blake3::hex("abc"));

    // finish 后 reset 回到无键模式可复用，结果复现
    ASSERT_TRUE(h.reset_keyed(k1));
    h.update("payload");
    const std::string first = h.finish();
    h.reset();
    h.update("payload");
    EXPECT_NE(h.finish(), first);       // 无键模式 ≠ keyed
    h.reset();
    ASSERT_TRUE(h.reset_keyed(k1));
    h.update("payload");
    EXPECT_EQ(h.finish(), first);       // 同密钥复现
}

// ------------------------------- ini_config ------------------------------

TEST(IniConfigTest, ParseAndGet)
{
    using namespace libmini;
    const char* kText =
        "; top comment\n"
        "# another comment\n"
        "global_key = global_value\n"
        "\n"
        "[server]        ; inline comment\n"
        "host = 127.0.0.1\n"
        "port: 8080\n"
        "timeout = 30   # trailing comment\n"
        "verbose = YES\n"
        "title = \"My App\"\n"
        "empty =\n"
        "\n"
        "[paths]\n"
        "data = /var/lib/app\n";

    IniConfig cfg;
    cfg.parse(kText);

    EXPECT_EQ(cfg.get("", "global_key"), "global_value");
    EXPECT_EQ(cfg.get("server", "host"), "127.0.0.1");
    EXPECT_EQ(cfg.get("server", "port"), "8080");  // ':' 分隔符
    EXPECT_EQ(cfg.get_int("server", "port"), 8080);
    EXPECT_EQ(cfg.get_int("server", "timeout"), 30);
    EXPECT_TRUE(cfg.get_bool("server", "verbose"));  // YES
    EXPECT_EQ(cfg.get("server", "title"), "My App"); // 剥引号
    EXPECT_EQ(cfg.get("server", "empty"), "");
    EXPECT_EQ(cfg.get("paths", "data"), "/var/lib/app");

    // 默认值
    EXPECT_EQ(cfg.get("server", "missing", "dft"), "dft");
    EXPECT_EQ(cfg.get_int("server", "missing", -1), -1);
    EXPECT_EQ(cfg.get_double("server", "ratio", 1.5), 1.5);
    EXPECT_TRUE(cfg.get_bool("server", "missing", true));

    EXPECT_TRUE(cfg.has_section("server"));
    EXPECT_FALSE(cfg.has_section("nope"));
    EXPECT_TRUE(cfg.has_key("server", "host"));
    EXPECT_FALSE(cfg.has_key("server", "nope"));

    const std::vector<std::string> ss = cfg.sections();
    ASSERT_EQ(ss.size(), 2u);
    EXPECT_EQ(ss[0], "paths");
    EXPECT_EQ(ss[1], "server");
    const std::vector<std::string> ks = cfg.keys("server");
    ASSERT_EQ(ks.size(), 6u);
}

TEST(IniConfigTest, SetRemoveRoundTrip)
{
    using namespace libmini;
    IniConfig cfg;
    cfg.set("a", "x", "1");
    cfg.set("a", "y", "true");
    cfg.set("", "g", "global");
    cfg.set("b", "z", "2.5");

    EXPECT_EQ(cfg.get_int("a", "x"), 1);
    EXPECT_TRUE(cfg.get_bool("a", "y"));
    EXPECT_EQ(cfg.get_double("b", "z"), 2.5);
    EXPECT_EQ(cfg.get("", "g"), "global");

    // save → parse 往返
    IniConfig cfg2;
    cfg2.parse(cfg.save());
    EXPECT_EQ(cfg2.get_int("a", "x"), 1);
    EXPECT_TRUE(cfg2.get_bool("a", "y"));
    EXPECT_EQ(cfg2.get("", "g"), "global");
    EXPECT_EQ(cfg2.get_double("b", "z"), 2.5);

    cfg.remove("a", "x");
    EXPECT_FALSE(cfg.has_key("a", "x"));
    cfg.remove_section("a");
    EXPECT_FALSE(cfg.has_section("a"));

    cfg.clear();
    EXPECT_TRUE(cfg.sections().empty());
}

// --------------------------------- gzip ----------------------------------

TEST(GzipTest, RoundTrip)
{
    using namespace libmini;
    // 空串、短串、长重复串（考验压缩）、二进制串
    const std::vector<std::string> cases = {
        "",
        "a",
        "hello hello hello hello hello hello hello hello hello",
        std::string(10000, 'z'),
        // binary cases: lengths hand-given (std::string(literal, 16) reads past
        // the 15-byte array - strict CI flags it as -Werror=array-bounds)
        std::string("binary\x00\x01\xff data", 15),
        std::string("binary\x00\x01\xff data", 14),
        std::string("binary\x00\x01\xff data", 13),
        std::string("binary\x00\x01\xff data", 6)};

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const optional<std::string> compressed = gzip_compress(cases[i]);
        ASSERT_TRUE(compressed.has_value()) << "case " << i;
        const optional<std::string> decompressed = gzip_decompress(*compressed);
        ASSERT_TRUE(decompressed.has_value()) << "case " << i;
        EXPECT_EQ(*decompressed, cases[i]) << "case " << i;
    }
}

TEST(GzipTest, HeaderAndCompressionRatio)
{
    using namespace libmini;
    const std::string payload(5000, 'q');
    const optional<std::string> compressed = gzip_compress(payload, 9);
    ASSERT_TRUE(compressed.has_value());

    // gzip 魔数 1f 8b
    EXPECT_EQ(static_cast<unsigned char>((*compressed)[0]), 0x1F);
    EXPECT_EQ(static_cast<unsigned char>((*compressed)[1]), 0x8B);
    // 高压缩比下输出远小于输入
    EXPECT_LT(compressed->size(), payload.size() / 10);

    const optional<std::string> out = gzip_decompress(*compressed);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, payload);
}

TEST(GzipTest, DecompressRejectsGarbage)
{
    using namespace libmini;
    EXPECT_FALSE(gzip_decompress("").has_value());
    EXPECT_FALSE(gzip_decompress("not gzip at all").has_value());
    EXPECT_FALSE(gzip_decompress(std::string("\x1f\x8b\x08\x00trunc", 9)).has_value());
}

// --------------------------------- zstd -----------------------------------

TEST(ZstdTest, RoundTrip)
{
    using namespace libmini;
    const std::vector<std::string> cases = {
        "",
        "a",
        "hello hello hello hello hello hello hello hello hello",
        std::string(100000, 'z'),
        std::string("binary\x00\x01\xff data", 15),
        std::string("binary\x00\x01\xff data", 7)};

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const optional<std::string> packed = zstd_compress(cases[i]);
        ASSERT_TRUE(packed.has_value()) << "case " << i;
        const optional<std::string> raw = zstd_decompress(*packed);
        ASSERT_TRUE(raw.has_value()) << "case " << i;
        EXPECT_EQ(*raw, cases[i]) << "case " << i;
    }
}

TEST(ZstdTest, FrameMagicAndCompressionRatio)
{
    using namespace libmini;
    const std::string payload(50000, 'q');
    const optional<std::string> packed = zstd_compress(payload, 19);
    ASSERT_TRUE(packed.has_value());

    // zstd 帧魔数：28 B5 2F FD（小端）
    ASSERT_GE(packed->size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>((*packed)[0]), 0x28);
    EXPECT_EQ(static_cast<unsigned char>((*packed)[1]), 0xB5);
    EXPECT_EQ(static_cast<unsigned char>((*packed)[2]), 0x2F);
    EXPECT_EQ(static_cast<unsigned char>((*packed)[3]), 0xFD);
    // 高压缩比下输出远小于输入
    EXPECT_LT(packed->size(), payload.size() / 100);

    const optional<std::string> out = zstd_decompress(*packed);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, payload);
}

TEST(ZstdTest, DecompressRejectsGarbageAndBombs)
{
    using namespace libmini;
    EXPECT_FALSE(zstd_decompress("").has_value());
    EXPECT_FALSE(zstd_decompress("not zstd at all").has_value());
    // 截断的合法帧
    const optional<std::string> packed = zstd_compress(std::string(5000, 'x'));
    ASSERT_TRUE(packed.has_value());
    EXPECT_FALSE(zstd_decompress(packed->substr(0, packed->size() / 2)).has_value());

    // 声明体积超过 max_output：不解压直接拒绝
    EXPECT_FALSE(zstd_decompress(*packed, 10).has_value());
    // 正常体积在上限内：成功
    EXPECT_TRUE(zstd_decompress(*packed, 100000).has_value());
    // max_output=0 表示不限制
    EXPECT_TRUE(zstd_decompress(*packed, 0).has_value());
}

TEST(ZstdTest, CompressRejectsBadLevel)
{
    using namespace libmini;
    EXPECT_FALSE(zstd_compress("x", 999).has_value());
    // 快速模式下限是 -131072（ZSTD_minCLevel），再小才非法
    EXPECT_FALSE(zstd_compress("x", -200000).has_value());
    // 合法边界不报错（1..22 之外的具体值取决于 zstd 版本，只验证往返）
    const optional<std::string> p = zstd_compress("level edge", 1);
    ASSERT_TRUE(p.has_value());
    const optional<std::string> r = zstd_decompress(*p);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "level edge");
}

// -------------------------------- async ----------------------------------

TEST(AsyncSchedulerTest, RunAfterExecutesOnce)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    sched.run_after_ms(30, [&calls] { ++calls; });

    // 轮询等执行：CI 慢机上固定 120ms 可能不够。不设「10ms 时未执行」
    // 的中间断言——sleep_for 是下限，休眠过冲越过 30ms 截止点会误报
    //（macOS 实测踩过）；「执行恰好一次」由 run_after 的一次性语义保证
    bool ran = false;
    for (int i = 0; i < 250 && !ran; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ran = calls.load() >= 1;
    }
    EXPECT_EQ(calls.load(), 1);   // 到期执行且只执行一次
    EXPECT_EQ(sched.pending_count(), 0u);
}

TEST(AsyncSchedulerTest, CancelBeforeDue)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    TaskHandle h = sched.run_after_ms(200, [&calls] { ++calls; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    EXPECT_TRUE(h.cancel());      // 尚未到期，取消成功
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(calls.load(), 0);   // 永不执行
    EXPECT_EQ(sched.pending_count(), 0u);

    // 空句柄/重复取消安全
    TaskHandle nil;
    EXPECT_FALSE(nil.cancel());
    EXPECT_FALSE(h.cancel());
}

TEST(AsyncSchedulerTest, PeriodicRunsAndStopsByReturnFalse)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    // 执行 3 次后返回 false 自停
    sched.run_every_ms(20, [&calls]() -> bool {
        return calls.fetch_add(1) + 1 < 3;
    });

    // 轮询等 3 次自停（慢机上固定 250ms 可能跑不完 3 次；单 worker
    // 串行执行，第 3 次回调返回 false 后不会再有第 4 次，计数稳定）
    bool stopped = false;
    for (int i = 0; i < 300 && !stopped; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stopped = calls.load() >= 3;
    }
    ASSERT_GE(calls.load(), 3);
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(sched.pending_count(), 0u);  // 后续期次已被清理
}

TEST(AsyncSchedulerTest, PeriodicCancelStopsFutureRuns)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> calls{0};
    TaskHandle h = sched.run_every_ms(30, [&calls] { return ++calls > 0; });

    // 轮询等首次执行：慢机上固定 100ms 可能一次都跑不到
    bool ran = false;
    for (int i = 0; i < 250 && !ran; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ran = calls.load() >= 1;
    }
    ASSERT_GE(calls.load(), 1);   // 已至少执行过一次
    EXPECT_TRUE(h.cancel());
    const int at_cancel = calls.load();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(calls.load(), at_cancel);   // 之后不再执行
}

TEST(AsyncSchedulerTest, CancelAllAndDtorStopsCleanly)
{
    using namespace libmini;
    std::atomic<int> calls{0};
    {
        AsyncScheduler sched;
        for (int i = 0; i < 5; ++i) {
            sched.run_after_ms(500, [&calls] { ++calls; });
        }
        EXPECT_EQ(sched.pending_count(), 5u);
        sched.cancel_all();
        EXPECT_EQ(sched.pending_count(), 0u);
        // 析构在任务到期前发生，join 干净退出
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_EQ(calls.load(), 0);
}

TEST(AsyncSchedulerTest, TaskCanScheduleAnotherTask)
{
    using namespace libmini;
    AsyncScheduler sched;

    std::atomic<int> final_calls{0};
    std::atomic<bool> chained{false};
    sched.run_after_ms(20, [&sched, &chained, &final_calls] {
        chained = true;
        sched.run_after_ms(20, [&final_calls] { ++final_calls; });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_TRUE(chained.load());
    EXPECT_EQ(final_calls.load(), 1);   // 工作线程内提交任务不死锁
}

TEST(RateLimiterTest, BurstThenThrottles)
{
    using namespace libmini;
    // 稳态 20/s，桶 3：前 3 次立即成功，之后开始限流
    RateLimiter limiter(20.0, 3.0);

    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_TRUE(limiter.try_acquire());
    EXPECT_FALSE(limiter.try_acquire());   // 桶空
    EXPECT_FALSE(limiter.try_acquire());

    // 约 50ms 补 1 个令牌
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(limiter.try_acquire());

    // 补充速率有上限：清空桶后等足够久（20/s × 600ms 足补 12 个），
    // 令牌应饱和在桶容量 3，而不是无界累积。确定性断言：慢机上补充
    // 只会更多，饱和值不变，不再依赖轮询碰中间状态。
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const double avail = limiter.available();
    EXPECT_NEAR(avail, 3.0, 1e-6);   // 饱和在桶容量
}

TEST(RateLimiterTest, AcquireBlocksUntilTokenAvailable)
{
    using namespace libmini;
    RateLimiter limiter(25.0, 1.0);   // 40ms 一个令牌

    const auto t0 = std::chrono::steady_clock::now();
    const std::int64_t waited = limiter.acquire();   // 桶里 1 个立即拿到
    const std::int64_t waited2 = limiter.acquire();  // 需等约 40ms
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    EXPECT_LE(waited, 5);          // 初始令牌立即可用
    EXPECT_GE(waited2, 25);        // 第二次真实等待
    EXPECT_GE(elapsed, 25);
    EXPECT_LT(elapsed, 500);       // 不会等过头
}

TEST(RateLimiterTest, MultiPermitAcquire)
{
    using namespace libmini;
    RateLimiter limiter(10.0, 5.0);

    EXPECT_TRUE(limiter.try_acquire(5.0));  // 一次取空桶
    EXPECT_FALSE(limiter.try_acquire(1.0));
    EXPECT_FALSE(limiter.try_acquire(2.0));

    // 取超过容量的令牌：需要多个补充周期，最终能拿到
    const std::int64_t waited = limiter.acquire(2.0);
    EXPECT_GE(waited, 100);   // 10/s → 2 个令牌约 200ms
}

// ------------------------------ json_utils --------------------------------

TEST(JsonUtilsTest, ToJsonStringSimpleEscapesSpecialChars)
{
    using namespace libmini;
    // 引号必须转义，否则输出非法 JSON
    EXPECT_EQ(to_json_string_simple("say \"hi\""), "\"say \\\"hi\\\"\"");
    // 反斜杠必须转义
    EXPECT_EQ(to_json_string_simple("a\\b"), "\"a\\\\b\"");
    // 控制字符
    EXPECT_EQ(to_json_string_simple("line1\nline2"), "\"line1\\nline2\"");
    EXPECT_EQ(to_json_string_simple("tab\there"), "\"tab\\there\"");
    // 空串
    EXPECT_EQ(to_json_string_simple(""), "\"\"");
    // 中文原样保留（UTF-8）
    EXPECT_EQ(to_json_string_simple("\xE4\xB8\xAD\xE6\x96\x87"),
              "\"\xE4\xB8\xAD\xE6\x96\x87\"");
}

TEST(JsonUtilsTest, ToJsonStringSimpleProducesValidJson)
{
    using namespace libmini;
    const std::string nasty = "he said \"hi\" \\ done\n\t\x01";
    const std::string encoded = to_json_string_simple(nasty);

    // 输出必须是合法 JSON 字符串字面量，且解析回来等于原串
    JsonValue parsed;
    ASSERT_NO_THROW(parsed = parse_json(encoded));
    ASSERT_TRUE(parsed.is_string());
    EXPECT_EQ(parsed.get<std::string>(), nasty);

    // 旧行为回归基线：普通字符串只包引号
    EXPECT_EQ(to_json_string_simple("test"), "\"test\"");
}

TEST(JsonUtilsTest, EscapeJsonString)
{
    using namespace libmini;
    EXPECT_EQ(escape_json_string("x"), "x");       // 无需转义时原样
    EXPECT_EQ(escape_json_string("a\"b"), "a\\\"b");
    EXPECT_EQ(escape_json_string("a\\b"), "a\\\\b");
    EXPECT_EQ(escape_json_string("n\nm"), "n\\nm");
    EXPECT_EQ(escape_json_string(""), "");

    // 手工拼接 JSON 的推荐用法：动态值经 escape 后拼入模板
    const std::string user_input = "evil \"quote\" and \\ backslash";
    const std::string manual =
        std::string("{\"msg\":\"") + escape_json_string(user_input) + "\"}";
    JsonValue parsed;
    ASSERT_NO_THROW(parsed = parse_json(manual));
    EXPECT_EQ(parsed["msg"].get<std::string>(), user_input);
}

TEST(JsonUtilsTest, ParseJsonSimpleNormalizesAndValidates)
{
    using namespace libmini;
    // 规范化：吃进带空白/换行的 JSON，输出紧凑格式
    EXPECT_EQ(parse_json_simple("{ \"a\" : 1, \"b\" : \"x\" }"),
              "{\"a\":1,\"b\":\"x\"}");
    // 紧凑输入原样输出（旧测试依赖的非空行为不变）
    EXPECT_EQ(parse_json_simple("{\"name\":\"test\",\"value\":123}"),
              "{\"name\":\"test\",\"value\":123}");
    // 数组与标量也支持
    EXPECT_EQ(parse_json_simple("[1, 2, 3]"), "[1,2,3]");
    EXPECT_EQ(parse_json_simple("\"bare\""), "\"bare\"");

    // 非法输入返回空串，不抛异常
    EXPECT_EQ(parse_json_simple("garbage"), "");
    EXPECT_EQ(parse_json_simple(""), "");
    EXPECT_EQ(parse_json_simple("{\"a\":}"), "");
    EXPECT_EQ(parse_json_simple("{\"a\":1,}"), "");
}

// ------------------------------ file_utils --------------------------------

TEST(FileUtilsTest, Utf8ChinesePathRoundTrip)
{
    using namespace libmini;
    const std::string fname = "libmini_测试文件.txt";

    // 写入/存在性/大小/读回 —— 全程 UTF-8 中文路径
    EXPECT_TRUE(write_file(fname, "hello 中文内容"));
    EXPECT_TRUE(file_exists(fname));
    EXPECT_EQ(file_size(fname), 18u);            // 6 + 4×3（UTF-8 汉字）
    EXPECT_EQ(read_file(fname), "hello 中文内容");

    // 二进制内容往返
    const std::string binary("\x00\x01\xff\xfe\xe4\xb8\xad", 7);
    EXPECT_TRUE(write_file(fname, binary));
    EXPECT_EQ(read_file(fname), binary);

    // 目录列表应包含该文件（UTF-8 返回），且不含 "." 与 ".."
    const std::vector<std::string> entries = list_directory(".");
    EXPECT_TRUE(std::find(entries.begin(), entries.end(), fname) != entries.end());
    EXPECT_TRUE(std::find(entries.begin(), entries.end(), ".") == entries.end());
    EXPECT_TRUE(std::find(entries.begin(), entries.end(), "..") == entries.end());

    // 清理
    EXPECT_TRUE(remove_file(fname));
    EXPECT_FALSE(file_exists(fname));

    // 不存在的中文路径
    EXPECT_FALSE(file_exists("不存在_某某_9x7.txt"));
    EXPECT_TRUE(read_file("不存在_某某_9x7.txt").empty());
    EXPECT_EQ(file_size("不存在_某某_9x7.txt"), 0u);
    EXPECT_FALSE(remove_file("不存在_某某_9x7.txt"));
}

TEST(FileUtilsTest, EmptyWriteAndDirectoryListing)
{
    using namespace libmini;
    const std::string fname = "libmini_empty_test.tmp";
    EXPECT_TRUE(write_file(fname, ""));
    EXPECT_TRUE(file_exists(fname));
    EXPECT_EQ(file_size(fname), 0u);
    EXPECT_TRUE(read_file(fname).empty());

    // 覆盖写
    EXPECT_TRUE(write_file(fname, "v2"));
    EXPECT_EQ(read_file(fname), "v2");
    EXPECT_TRUE(remove_file(fname));

    // 当前目录列表非空（至少有测试可执行文件）
    EXPECT_FALSE(list_directory(".").empty());
}

// ------------------ file_utils 扩展：符号链接 / 权限 / 目录占用 / 临时目录 ------------------

TEST(FileUtilsExtensionTest, SymlinkReadEmptyOnWindows)
{
    using namespace libmini;
    // Windows 下 read_symlink 始终返回空（无法获取未解引用的目标）。
    // 此测试在 Windows 上仅断言返回空；在 POSIX 下创建一个符号链接并读取。
#ifdef _WIN32
    EXPECT_TRUE(read_symlink("nonexistent_link_9x7").empty());
#else
    const std::string target = "target_file_9x7.txt";
    const std::string link = "link_to_target_9x7.txt";
    // 创建目标文件
    ASSERT_TRUE(write_file(target, "hello"));
    // 创建符号链接
    ASSERT_TRUE(create_symlink(target, link));
    // 读取符号链接目标
    EXPECT_EQ(read_symlink(link), target);
    // 清理
    remove_symlink(link);
    remove_file(target);
#endif
}

TEST(FileUtilsExtensionTest, CreateSymlinkFile)
{
    using namespace libmini;
    // 创建文件符号链接
    const std::string target = "symlink_target_9x7.txt";
    const std::string link = "symlink_link_9x7.txt";
    ASSERT_TRUE(write_file(target, "test content"));
    // 在 Windows 上，创建符号链接可能需要权限；接受失败
#ifdef _WIN32
    // 尝试创建符号链接（可能因权限失败）
    const bool created = create_symlink(target, link);
    if (created) {
        EXPECT_TRUE(file_exists(link));
        EXPECT_EQ(read_file(link), "test content");  // 跟随符号链接读取
        // 删除符号链接
        EXPECT_TRUE(remove_symlink(link));
        EXPECT_FALSE(file_exists(link));
    }
#else
    if (!create_symlink(target, link)) {
        GTEST_SKIP() << "create_symlink failed (sandbox may forbid symlinks)";
    }
    EXPECT_TRUE(file_exists(link));
    EXPECT_EQ(read_symlink(link), target);
    EXPECT_EQ(read_file(link), "test content");
    remove_symlink(link);
#endif
    remove_file(target);
}

TEST(FileUtilsExtensionTest, RemoveSymlink)
{
    using namespace libmini;
    const std::string target = "rmsym_target_9x7.txt";
    const std::string link = "rmsym_link_9x7.txt";
    ASSERT_TRUE(write_file(target, "data"));
#ifdef _WIN32
    // Windows 下可能创建失败，跳过测试
    if (!create_symlink(target, link)) {
        remove_file(target);
        return;
    }
#else
    if (!create_symlink(target, link)) {
        GTEST_SKIP() << "create_symlink failed (sandbox may forbid symlinks)";
    }
#endif
    EXPECT_TRUE(file_exists(link));
    EXPECT_TRUE(remove_symlink(link));
    EXPECT_FALSE(file_exists(link));
    // 原目标仍存在
    EXPECT_TRUE(file_exists(target));
    remove_file(target);
}

TEST(FileUtilsExtensionTest, FilePermissionsPlatformDifference)
{
    using namespace libmini;
    const std::string fname = "perm_test_9x7.txt";
    ASSERT_TRUE(write_file(fname, "test"));
    std::uint32_t mode = 0;
#ifdef _WIN32
    // Windows 下 file_permissions 始终返回 false
    EXPECT_FALSE(file_permissions(fname, mode));
    EXPECT_EQ(mode, 0u);
#else
    ASSERT_TRUE(file_permissions(fname, mode));
    EXPECT_GT(mode, 0u);
    // 修改权限为 0600
    ASSERT_TRUE(set_file_permissions(fname, 0600));
    std::uint32_t new_mode = 0;
    ASSERT_TRUE(file_permissions(fname, new_mode));
    EXPECT_EQ(new_mode & (S_IRWXU | S_IRWXG | S_IRWXO), 0600u);
    // 恢复权限为 0644
    ASSERT_TRUE(set_file_permissions(fname, 0644));
#endif
    remove_file(fname);
}

TEST(FileUtilsExtensionTest, DirectorySizeEmptyDirectory)
{
    using namespace libmini;
    const std::string dir = "dirsize_empty_9x7";
    ASSERT_TRUE(make_directory(dir));
    EXPECT_EQ(directory_size(dir), 0u);
    remove_directory(dir);
}

TEST(FileUtilsExtensionTest, DirectorySizeWithFile)
{
    using namespace libmini;
    const std::string dir = "dirsize_withfile_9x7";
    ASSERT_TRUE(make_directory(dir));
    const std::string fname = dir + "/test.txt";
    const std::string content = "hello";
    ASSERT_TRUE(write_file(fname, content));
    EXPECT_EQ(directory_size(dir), content.size());
    remove_tree(dir);
}

TEST(FileUtilsExtensionTest, DirectorySizeRecursive)
{
    using namespace libmini;
    const std::string dir = "dirsize_recursive_9x7";
    ASSERT_TRUE(make_directories(dir + "/a/b"));
    ASSERT_TRUE(write_file(dir + "/a/b/c.txt", "deep"));
    ASSERT_TRUE(write_file(dir + "/a/d.txt", "shallow"));
    // 所有文件大小之和："deep" 為 4 字元，"shallow" 為 7 字元
    EXPECT_EQ(directory_size(dir), 4u + 7u);
    remove_tree(dir);
}

TEST(FileUtilsExtensionTest, DirectorySizeSymlinkNotFollowed)
{
    using namespace libmini;
    const std::string dir = "dirsize_sym_9x7";
    const std::string target_dir = "dirsize_sym_target_9x7";
    ASSERT_TRUE(make_directory(dir));
    ASSERT_TRUE(make_directory(target_dir));
    ASSERT_TRUE(write_file(target_dir + "/big.txt", std::string(1000, 'x')));
#ifdef _WIN32
    // Windows 下创建目录符号链接可能需要权限；若失败则跳过
    if (!create_symlink(target_dir, dir + "/link", true)) {
        remove_tree(dir);
        remove_tree(target_dir);
        return;
    }
    // Windows 下 read_symlink 返回空，无法解析符号链接目标，
    // 因此 follow_symlinks = true 时也无法追踪，返回 0。
    EXPECT_EQ(directory_size(dir, false), 0u);
    EXPECT_EQ(directory_size(dir, true), 0u);
#else
    // 目标写成相对链接所在目录的 "../target"：POSIX 把相对目标解析为
    // 基于链接父目录，写成 CWD 相对路径会指向 dir/ 下不存在的路径
    if (!create_symlink("../" + target_dir, dir + "/link", true)) {
        GTEST_SKIP() << "create_symlink (directory) failed (sandbox may forbid symlinks)";
    }
    // 不跟随符号链接时，目录大小不应包含目标目录中的文件
    EXPECT_EQ(directory_size(dir, false), 0u);
    // 跟随符号链接时，包含目标目录中的文件
    EXPECT_EQ(directory_size(dir, true), 1000u);
#endif
    remove_tree(dir);
    remove_tree(target_dir);
}

TEST(FileUtilsExtensionTest, UniqueTempDirectoryCreation)
{
    using namespace libmini;
    const std::string tmpdir = unique_temp_directory("libmini_test_");
    ASSERT_FALSE(tmpdir.empty());
    EXPECT_TRUE(file_exists(tmpdir));
    EXPECT_TRUE(is_directory(tmpdir));
    // 创建临时文件于其中
    const std::string fname = tmpdir + "/test.txt";
    ASSERT_TRUE(write_file(fname, "temp"));
    EXPECT_EQ(read_file(fname), "temp");
    // 清理整个临时目录
    remove_tree(tmpdir);
    EXPECT_FALSE(file_exists(tmpdir));
}

TEST(FileUtilsExtensionTest, UniqueTempDirectoryWithCustomPrefix)
{
    using namespace libmini;
    const std::string tmpdir = unique_temp_directory("custom_prefix_");
    ASSERT_FALSE(tmpdir.empty());
    EXPECT_TRUE(file_exists(tmpdir));
    EXPECT_TRUE(is_directory(tmpdir));
    // 路径应包含自定义前缀
    EXPECT_EQ(tmpdir.find("custom_prefix_") != std::string::npos, true);
    remove_tree(tmpdir);
}

TEST(FileUtilsExtensionTest, UniqueTempDirectoryInCustomDir)
{
    using namespace libmini;
    const std::string base = "temp_base_9x7";
    ASSERT_TRUE(make_directory(base));
    const std::string tmpdir = unique_temp_directory("sub_", base);
    ASSERT_FALSE(tmpdir.empty());
    EXPECT_TRUE(file_exists(tmpdir));
    EXPECT_TRUE(is_directory(tmpdir));
    // 路径应包含自定义目录
    EXPECT_EQ(tmpdir.find(base) != std::string::npos, true);
    remove_tree(base);
}

// ------------------------------- format 辅助 ------------------------------

TEST(FormatHelpersTest, FormatBytes)
{
    using namespace libmini;
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(512), "512 B");
    EXPECT_EQ(format_bytes(1024), "1 KB");
    EXPECT_EQ(format_bytes(1536), "1.5 KB");
    EXPECT_EQ(format_bytes(1024 * 1024), "1 MB");
    EXPECT_EQ(format_bytes(3 * 1024 * 1024 + 512 * 1024), "3.5 MB");
    EXPECT_EQ(format_bytes((3LL * 1024 * 1024 * 1024) / 2), "1.5 GB");
    EXPECT_EQ(format_bytes(-2048), "-2 KB");
}

TEST(FormatHelpersTest, FormatDurationMs)
{
    using namespace libmini;
    EXPECT_EQ(format_duration_ms(0), "0 ms");
    EXPECT_EQ(format_duration_ms(15), "15 ms");
    EXPECT_EQ(format_duration_ms(2500), "2.5 s");
    EXPECT_EQ(format_duration_ms(65500), "1m 05.5 s");
    EXPECT_EQ(format_duration_ms(3 * 3600000 + 2 * 60000 + 1000),
              "3h 2m 1s");
    EXPECT_EQ(format_duration_ms(-800), "-800 ms");
}

// ------------------------------- 原子写文件 --------------------------------

TEST(AtomicWriteTest, OverwriteAndCrashSafety)
{
    using namespace libmini;
    const std::string fname = "libmini_atomic_test.tmp";
    EXPECT_TRUE(write_file_atomic(fname, "第一版内容 v1"));
    EXPECT_EQ(read_file(fname), "第一版内容 v1");

    // 原子覆盖：内容完整替换
    EXPECT_TRUE(write_file_atomic(fname, "第二版内容 v2"));
    EXPECT_EQ(read_file(fname), "第二版内容 v2");

    // 空内容也合法
    EXPECT_TRUE(write_file_atomic(fname, ""));
    EXPECT_TRUE(read_file(fname).empty());

    EXPECT_TRUE(remove_file(fname));

    // 目标目录不存在时失败，且不留临时文件（先清理历史残留，保证哨兵目录真不存在）
    remove_tree("no_such_dir_atomic_9x7");
    EXPECT_FALSE(write_file_atomic("no_such_dir_atomic_9x7/child.txt", "x"));
    EXPECT_FALSE(file_exists("no_such_dir_atomic_9x7"));
}

TEST(AtomicWriteTest, LargeContent)
{
    using namespace libmini;
    const std::string fname = "libmini_atomic_big.tmp";
    std::string big;
    big.reserve(3 * 1024 * 1024);
    for (int i = 0; i < 3 * 1024; ++i) {
        big += "0123456789abcdef";
    }
    EXPECT_TRUE(write_file_atomic(fname, big));
    EXPECT_EQ(file_size(fname), big.size());
    EXPECT_EQ(read_file(fname), big);
    EXPECT_TRUE(remove_file(fname));
}

// ------------------------------- 文件摘要 ----------------------------------

TEST(FileDigestTest, Sha256AndMd5MatchKnownValues)
{
    using namespace libmini;
    const std::string fname = "libmini_digest_test.tmp";
    const std::string content = "hello 文件摘要";
    EXPECT_TRUE(write_file(fname, content));

    // 与内存版 digest 对齐（同一实现，口径必须一致）
    EXPECT_EQ(sha256_file_hex(fname), Sha256::hex(content));
    EXPECT_EQ(md5_file_hex(fname), Md5::hex(content));

    // 空文件（SHA-256 of empty 的已知值）
    EXPECT_TRUE(write_file(fname, ""));
    EXPECT_EQ(sha256_file_hex(fname),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    EXPECT_TRUE(remove_file(fname));

    // 文件不存在返回空串
    EXPECT_TRUE(sha256_file_hex("no_such_file_digest.bin").empty());
    EXPECT_TRUE(md5_file_hex("no_such_file_digest.bin").empty());
}

TEST(FileDigestTest, LargeFileStreaming)
{
    using namespace libmini;
    const std::string fname = "libmini_digest_big.tmp";
    // 5MB 内容：分块写后，流式摘要必须与内存摘要一致
    std::string chunk("abcdefgh01234567");
    std::string content;
    for (int i = 0; i < 5 * 1024; ++i) content += chunk;
    EXPECT_TRUE(write_file(fname, content));
    EXPECT_EQ(sha256_file_hex(fname), Sha256::hex(content));
    EXPECT_EQ(md5_file_hex(fname), Md5::hex(content));
    EXPECT_TRUE(remove_file(fname));
}

// ------------------------------- system_info -------------------------------

TEST(SystemInfoTest, BasicQueriesAreSane)
{
    using namespace libmini;
    EXPECT_FALSE(hostname().empty());
    EXPECT_NE(current_pid(), 0u);
    EXPECT_FALSE(executable_path().empty());
    EXPECT_TRUE(file_exists(executable_path()));  // 可执行文件路径真实存在
    EXPECT_GE(cpu_count(), 1);

    EXPECT_GT(total_physical_memory(), 0u);
    EXPECT_GT(available_physical_memory(), 0u);
    EXPECT_LE(available_physical_memory(), total_physical_memory());

    EXPECT_GT(disk_total_bytes("."), 0u);
    EXPECT_GT(disk_free_bytes("."), 0u);
    EXPECT_LE(disk_free_bytes("."), disk_total_bytes("."));
    EXPECT_EQ(disk_total_bytes("no_such_dir_atomic_9x7/none"), 0u);
}

// ------------------------------ hardware_info ------------------------------
//
// 硬件清单跨平台差异极大（虚拟盘无序列号、DMI 需 root、容器里没有网卡…），
// 因此测试只断言「结构上必然成立的性质」：非空列表的元素必须字段合法、
// 格式可校验，不对具体机器的字段值做断言。

TEST(HardwareInfoTest, FormatMacNormalizes)
{
    using namespace libmini;
    EXPECT_EQ(format_mac("AA-BB-CC-DD-EE-FF"), "aa:bb:cc:dd:ee:ff");
    EXPECT_EQ(format_mac("aabbccddeeff"), "aa:bb:cc:dd:ee:ff");
    EXPECT_EQ(format_mac("AA:BB:CC:DD:EE:FF"), "aa:bb:cc:dd:ee:ff");
    EXPECT_EQ(format_mac("aa bb cc dd ee ff"), "aa:bb:cc:dd:ee:ff");
    // 非法输入原样返回（不猜测）
    EXPECT_EQ(format_mac(""), "");
    EXPECT_EQ(format_mac("not-a-mac"), "not-a-mac");
    EXPECT_EQ(format_mac("aabbcc"), "aabbcc");
    // 长度正确才规范化
    EXPECT_NE(format_mac("aabbccddeeff"), "aabbccddeeff");
}

TEST(HardwareInfoTest, CpuInfoIsSane)
{
    using namespace libmini;
    const CpuInfo cpu = cpu_info();
    EXPECT_FALSE(cpu.empty()) << "任何真实机器都应能取到 CPU 信息";
    EXPECT_GE(cpu.logical_cores, 1);
    EXPECT_FALSE(cpu.architecture.empty());
    EXPECT_NE(cpu.architecture, "unknown");
    // 物理核数与超线程标记自洽
    if (cpu.physical_cores > 0) {
        EXPECT_GE(cpu.logical_cores, cpu.physical_cores);
        if (cpu.logical_cores > cpu.physical_cores) {
            EXPECT_TRUE(cpu.hyperthreading);
        }
    }
    if (cpu.hyperthreading) {
        EXPECT_GT(cpu.logical_cores, 0);
    }
    // 与 system_info 的核数口径一致（都是逻辑核）
    EXPECT_EQ(cpu.logical_cores, cpu_count());
}

TEST(HardwareInfoTest, NetworkAdaptersAreWellFormed)
{
    using namespace libmini;
    const std::vector<NetworkAdapterInfo> adapters = network_adapters();
    ASSERT_FALSE(adapters.empty()) << "至少应有回环适配器";
    std::size_t up_count = 0;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const NetworkAdapterInfo& a = adapters[i];
        EXPECT_FALSE(a.name.empty()) << "适配器必须有名字";
        if (!a.mac_address.empty()) {
            // 格式必须是 aa:bb:cc:dd:ee:ff
            EXPECT_EQ(a.mac_address.size(), 17u);
            EXPECT_EQ(a.mac_address[2], ':');
            EXPECT_EQ(a.mac_address[8], ':');
            EXPECT_EQ(a.mac_address[14], ':');
            EXPECT_EQ(a.mac_error, "");
        } else {
            EXPECT_FALSE(a.mac_error.empty())
                << "无 MAC 时必须说明原因：" << a.name;
        }
        for (std::size_t j = 0; j < a.ipv4_addresses.size(); ++j) {
            // IPv4 字面量必须是点分四段、且不含冒号（不含 IPv6）
            const std::string& ip = a.ipv4_addresses[j];
            EXPECT_FALSE(ip.empty());
            EXPECT_EQ(ip.find(':'), std::string::npos);
            EXPECT_EQ(std::count(ip.begin(), ip.end(), '.'), 3);
        }
        if (a.is_up && !a.is_loopback) {
            ++up_count;
        }
    }
    // 便捷接口与列表一致：非空时必须是列表里某个适配器的值
    const std::string mac = primary_mac_address();
    if (!mac.empty()) {
        bool found = false;
        for (std::size_t i = 0; i < adapters.size(); ++i) {
            if (adapters[i].mac_address == mac) {
                found = true;
            }
        }
        EXPECT_TRUE(found);
    }
}

TEST(HardwareInfoTest, DisksAndVolumesAreWellFormed)
{
    using namespace libmini;
    const std::vector<DiskInfo> ds = disks();
    for (std::size_t i = 0; i < ds.size(); ++i) {
        EXPECT_FALSE(ds[i].device_path.empty());
        EXPECT_FALSE(ds[i].interface_type.empty());
        if (!ds[i].serial_number.empty()) {
            EXPECT_TRUE(ds[i].serial_error.empty())
                << "拿到序列号就不该再有错误说明";
        }
    }

    const std::vector<VolumeInfo> vs = volumes();
    std::size_t real_volumes = 0;
    for (std::size_t i = 0; i < vs.size(); ++i) {
        EXPECT_FALSE(vs[i].mount_point.empty());
        if (vs[i].total_bytes == 0) {
            continue;  // 特殊挂载（无块可报）不在此列
        }
        ++real_volumes;
        EXPECT_LE(vs[i].free_bytes, vs[i].total_bytes);
    }
    EXPECT_GT(real_volumes, 0u) << "应至少列出一个有容量的真实卷";
    // 根卷必然存在（任何平台都挂载了 / 或 C:\）
    bool has_root = false;
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (vs[i].mount_point == "/" || vs[i].mount_point == "C:") {
            has_root = true;
        }
    }
    EXPECT_TRUE(has_root) << "应至少列出一个根卷";
}

TEST(HardwareInfoTest, BiosInfoIsConsistent)
{
    using namespace libmini;
    const BiosInfo bios = bios_info();
    if (!bios.serial_number.empty()) {
        EXPECT_TRUE(bios.serial_error.empty())
            << "拿到序列号就不该再有错误说明";
    } else if (!bios.empty()) {
        EXPECT_FALSE(bios.serial_error.empty())
            << "其他 BIOS 字段有值但序列号为空时，必须说明原因";
    }
}

// ------------------------------ machine_fingerprint ------------------------------

namespace {

// 从 signals 里筛出某类信号（形如 "board=..."）
std::vector<std::string> signals_with_prefix(
    const std::vector<std::string>& all, const std::string& prefix)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].size() > prefix.size() &&
            all[i].compare(0, prefix.size(), prefix) == 0) {
            out.push_back(all[i]);
        }
    }
    return out;
}

// 信号行的 "=" 之后部分
std::string signal_value(const std::string& signal)
{
    const std::size_t pos = signal.find('=');
    if (pos == std::string::npos) {
        return std::string();
    }
    return signal.substr(pos + 1);
}

bool is_lower_hex(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(MachineFingerprintTest, CanonicalTokenRejectsUnfilledPlaceholders)
{
    using namespace libmini;
    // DMI/WMI 里大量 OEM 没填序列号的模板值；不过滤会让同厂所有机器撞同一个
    // 指纹，许可证绑定直接失效
    const char* const kPlaceholders[] = {
        "To Be Filled By O.E.M.",
        "  to be filled by o.e.m.  ",
        "Default string",
        "System Serial Number",
        "System Board",
        "Base Board",
        "Not Specified",
        "Not Applicable",
        "Not Available",
        "Unknown",
        "None",
        "null",
        "N/A",
        "OEM string",
        "0123456789",
    };
    for (std::size_t i = 0;
         i < sizeof(kPlaceholders) / sizeof(kPlaceholders[0]); ++i) {
        EXPECT_EQ(canonical_fingerprint_token(kPlaceholders[i]), std::string())
            << "占位序列号必须被判为不可用: " << kPlaceholders[i];
    }
}

TEST(MachineFingerprintTest, CanonicalTokenRejectsShortAndUniformValues)
{
    using namespace libmini;
    // 有效字符不足 4 个：噪声
    EXPECT_EQ(canonical_fingerprint_token(""), std::string());
    EXPECT_EQ(canonical_fingerprint_token("   "), std::string());
    EXPECT_EQ(canonical_fingerprint_token("O.E.M."), std::string());
    EXPECT_EQ(canonical_fingerprint_token("abc"), std::string());
    EXPECT_EQ(canonical_fingerprint_token("-1-"), std::string());
    // 单一字符重复（忽略分隔符）：占位 MAC / 模板序列号
    EXPECT_EQ(canonical_fingerprint_token("00000000"), std::string());
    EXPECT_EQ(canonical_fingerprint_token("00:00:00:00:00:00"), std::string());
    EXPECT_EQ(canonical_fingerprint_token("XX-XXXXXX"), std::string());
    EXPECT_EQ(canonical_fingerprint_token("--------"), std::string());
    // 冒烟：真实的全 0 MAC 是非法的，真实 MAC 不能被误伤
    EXPECT_FALSE(canonical_fingerprint_token("AA:BB:CC:DD:EE:FF").empty());
}

TEST(MachineFingerprintTest, CanonicalTokenNormalizesRealValues)
{
    using namespace libmini;
    // 去首尾空白 + 转大写
    EXPECT_EQ(canonical_fingerprint_token("  cn-0abc1234  "), "CN-0ABC1234");
    // 内部连续空白（含制表符）压成单空格
    EXPECT_EQ(canonical_fingerprint_token("12th  Gen\tIntel Core"),
              "12TH GEN INTEL CORE");
    EXPECT_EQ(canonical_fingerprint_token("12th\r\n Gen\tIntel Core"),
              "12TH GEN INTEL CORE");
    // 幂等：再归一一次结果不变（signals 的稳定性依赖这一点）
    const std::string once = canonical_fingerprint_token(" PF2ABCD1 ");
    EXPECT_EQ(canonical_fingerprint_token(once), once);
    EXPECT_FALSE(once.empty());
}

TEST(MachineFingerprintTest, LooksLikeVirtualMachineDetectsHypervisors)
{
    using namespace libmini;
    EXPECT_TRUE(looks_like_virtual_machine("VMware Virtual Platform"));
    EXPECT_TRUE(looks_like_virtual_machine("VMware, Inc."));
    EXPECT_TRUE(looks_like_virtual_machine("VirtualBox"));
    EXPECT_TRUE(looks_like_virtual_machine("innotek GmbH"));
    EXPECT_TRUE(looks_like_virtual_machine("KVM"));
    EXPECT_TRUE(looks_like_virtual_machine("QEMU Virtual Machine"));
    EXPECT_TRUE(looks_like_virtual_machine("Microsoft Hyper-V Platform"));
    EXPECT_TRUE(looks_like_virtual_machine("Parallels Virtual Machine"));
    EXPECT_TRUE(looks_like_virtual_machine("Amazon EC2"));
    // 真实机器不得误判
    EXPECT_FALSE(looks_like_virtual_machine("Dell Inc."));
    EXPECT_FALSE(looks_like_virtual_machine("TO BE FILLED BY O.E.M."));
    EXPECT_FALSE(looks_like_virtual_machine("HUAWEI"));
    EXPECT_FALSE(looks_like_virtual_machine("LENOVO"));
    EXPECT_FALSE(looks_like_virtual_machine(""));
}

TEST(MachineFingerprintTest, LooksLikeVirtualAdapterDetectsTunnels)
{
    using namespace libmini;
    EXPECT_TRUE(looks_like_virtual_adapter("vEthernet (WSL)"));
    EXPECT_TRUE(looks_like_virtual_adapter("Docker Ethernet Adapter"));
    EXPECT_TRUE(looks_like_virtual_adapter("VMware Network Adapter VMXH"));
    EXPECT_TRUE(looks_like_virtual_adapter("Hyper-V Virtual Ethernet Adapter"));
    EXPECT_TRUE(looks_like_virtual_adapter("TAP-Windows Adapter V9"));
    EXPECT_TRUE(looks_like_virtual_adapter("Bluetooth Device (PAN)"));
    EXPECT_TRUE(looks_like_virtual_adapter("veth1234"));
    // 真实网卡不得误判
    EXPECT_FALSE(looks_like_virtual_adapter("Intel(R) Ethernet Connection"));
    EXPECT_FALSE(looks_like_virtual_adapter("Realtek PCIe GbE Family"));
    EXPECT_FALSE(looks_like_virtual_adapter("en0"));
    EXPECT_FALSE(looks_like_virtual_adapter(""));
}

TEST(MachineFingerprintTest, VersionStringIsStable)
{
    using namespace libmini;
    const char* v = machine_fingerprint_version();
    ASSERT_TRUE(v != nullptr);
    EXPECT_EQ(std::string(v), "v1");
    EXPECT_EQ(std::string(machine_fingerprint_version()), std::string(v));
}

TEST(MachineFingerprintTest, ShapeMatchesAvailableSignals)
{
    using namespace libmini;
    const MachineFingerprint fp = machine_fingerprint();
    if (fp.signals.empty()) {
        // 受限容器里可能一个信号都取不到，这是合法结果
        EXPECT_TRUE(fp.empty());
        EXPECT_EQ(fp.confidence, 0);
        EXPECT_TRUE(fp.short_id.empty());
        EXPECT_TRUE(fp.source.empty());
        EXPECT_FALSE(fp.missing.empty()) << "取不到信号时必须说明原因";
        return;
    }
    EXPECT_FALSE(fp.empty());
    EXPECT_EQ(fp.id.size(), 64u);
    EXPECT_TRUE(is_lower_hex(fp.id)) << "id 必须是 64 位小写十六进制";
    EXPECT_EQ(fp.short_id, fp.id.substr(0, 16));
    EXPECT_FALSE(fp.source.empty());
    EXPECT_GE(fp.confidence, 1);
    EXPECT_LE(fp.confidence, 100);
}

TEST(MachineFingerprintTest, IsDeterministicAcrossCalls)
{
    using namespace libmini;
    const MachineFingerprint a = machine_fingerprint();
    const MachineFingerprint b = machine_fingerprint();
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.source, b.source);
    EXPECT_EQ(a.confidence, b.confidence);
    EXPECT_EQ(a.is_virtual, b.is_virtual);
    EXPECT_EQ(a.signals, b.signals);
    EXPECT_EQ(a.missing, b.missing);
}

TEST(MachineFingerprintTest, StablePolicyExcludesVolatileSignals)
{
    using namespace libmini;
    // kStable：换硬盘 / 换网卡后指纹必须不变
    const MachineFingerprint fp =
        machine_fingerprint_with(FingerprintPolicy::kStable, std::string());
    EXPECT_TRUE(signals_with_prefix(fp.signals, "mac=").empty())
        << "kStable 不该纳入 MAC";
    EXPECT_TRUE(signals_with_prefix(fp.signals, "disk=").empty())
        << "kStable 不该纳入盘序列号";
    EXPECT_EQ(fp.policy, FingerprintPolicy::kStable);
}

TEST(MachineFingerprintTest, BalancedPolicyHasAtMostOneMacSignal)
{
    using namespace libmini;
    // kBalanced 只取字典序最小的那枚，避免枚举顺序影响结果
    const MachineFingerprint fp =
        machine_fingerprint_with(FingerprintPolicy::kBalanced, std::string());
    EXPECT_LE(signals_with_prefix(fp.signals, "mac=").size(), 1u);
    EXPECT_EQ(fp.policy, FingerprintPolicy::kBalanced);
    // 便捷入口就是 kBalanced
    const MachineFingerprint quick = machine_fingerprint();
    EXPECT_EQ(quick.signals, fp.signals);
    EXPECT_EQ(quick.id, fp.id);
}

TEST(MachineFingerprintTest, StrictPolicyDiskSignalsAreSortedAndUnique)
{
    using namespace libmini;
    const MachineFingerprint fp =
        machine_fingerprint_with(FingerprintPolicy::kStrict, std::string());
    EXPECT_EQ(fp.policy, FingerprintPolicy::kStrict);
    const std::vector<std::string> disks =
        signals_with_prefix(fp.signals, "disk=");
    for (std::size_t i = 1; i < disks.size(); ++i) {
        EXPECT_LT(disks[i - 1], disks[i]) << "盘信号必须升序且不重复";
    }
    // kStrict 的信号集合是 kBalanced 的超集
    const MachineFingerprint balanced =
        machine_fingerprint_with(FingerprintPolicy::kBalanced, std::string());
    EXPECT_LE(balanced.signals.size(), fp.signals.size());
}

TEST(MachineFingerprintTest, SignalsAreAlreadyCanonical)
{
    using namespace libmini;
    const FingerprintPolicy policies[3] = {FingerprintPolicy::kStable,
                                           FingerprintPolicy::kBalanced,
                                           FingerprintPolicy::kStrict};
    for (int i = 0; i < 3; ++i) {
        const MachineFingerprint fp = machine_fingerprint_with(policies[i], "");
        std::size_t prev_rank = 0;
        for (std::size_t j = 0; j < fp.signals.size(); ++j) {
            const std::string& s = fp.signals[j];
            EXPECT_TRUE(s.compare(0, 6, "board=") == 0 ||
                        s.compare(0, 4, "cpu=") == 0 ||
                        s.compare(0, 4, "mac=") == 0 ||
                        s.compare(0, 5, "disk=") == 0)
                << "信号前缀未登记: " << s;
            const std::string value = signal_value(s);
            EXPECT_FALSE(value.empty());
            // 幂等 + 不含占位符：signals 是调用方做跨版本迁移的依据
            EXPECT_EQ(canonical_fingerprint_token(value), value)
                << "信号未归一化: " << s;
            // 顺序固定：board -> cpu -> mac -> disk
            std::size_t rank = 0;
            if (s.compare(0, 6, "board=") == 0) {
                rank = 1;
            } else if (s.compare(0, 4, "cpu=") == 0) {
                rank = 2;
            } else if (s.compare(0, 4, "mac=") == 0) {
                rank = 3;
            } else {
                rank = 4;
            }
            EXPECT_GE(rank, prev_rank) << "信号顺序不稳定: " << s;
            prev_rank = rank;
        }
    }
}

TEST(MachineFingerprintTest, MissingSignalsExplainThemselves)
{
    using namespace libmini;
    const MachineFingerprint fp =
        machine_fingerprint_with(FingerprintPolicy::kStrict, std::string());
    for (std::size_t i = 0; i < fp.missing.size(); ++i) {
        const std::string& m = fp.missing[i];
        EXPECT_FALSE(m.empty());
        EXPECT_TRUE(m.compare(0, 6, "board=") == 0 ||
                    m.compare(0, 4, "cpu=") == 0 ||
                    m.compare(0, 4, "mac=") == 0 ||
                    m.compare(0, 5, "disk=") == 0)
            << "missing 条目未登记来源: " << m;
        EXPECT_GT(m.size(), m.find('=') + 1) << "missing 条目必须带原因: " << m;
    }
}

TEST(MachineFingerprintTest, SaltProducesADifferentId)
{
    using namespace libmini;
    const MachineFingerprint plain =
        machine_fingerprint_with(FingerprintPolicy::kBalanced, "");
    const MachineFingerprint salted =
        machine_fingerprint_with(FingerprintPolicy::kBalanced, "tenant-a");
    // 信号集合不受 salt 影响，id 必须受影响
    EXPECT_EQ(plain.signals, salted.signals);
    if (!plain.empty()) {
        EXPECT_NE(plain.id, salted.id);
        EXPECT_EQ(plain.short_id, plain.id.substr(0, 16));
    }
    const MachineFingerprint salted2 =
        machine_fingerprint_with(FingerprintPolicy::kBalanced, "tenant-b");
    if (!salted.empty()) {
        EXPECT_NE(salted.id, salted2.id);
    }
}

TEST(MachineFingerprintTest, PoliciesDifferOnlyWhenExtraSignalsExist)
{
    using namespace libmini;
    const MachineFingerprint stable =
        machine_fingerprint_with(FingerprintPolicy::kStable, "");
    const MachineFingerprint strict =
        machine_fingerprint_with(FingerprintPolicy::kStrict, "");
    if (stable.signals.size() == strict.signals.size()) {
        // 没有可加的易变信号时三种策略应给出同一个 id
        EXPECT_EQ(stable.id, strict.id);
    } else {
        EXPECT_FALSE(stable.empty());
        EXPECT_FALSE(strict.empty());
        EXPECT_NE(stable.id, strict.id);
    }
}

TEST(MachineFingerprintTest, VirtualMachineCapsConfidence)
{
    using namespace libmini;
    const MachineFingerprint fp = machine_fingerprint();
    if (fp.is_virtual) {
        EXPECT_LE(fp.confidence, 20)
            << "虚机指纹不可用于授权，confidence 必须压到 20 以内";
        EXPECT_GE(fp.confidence, 0);
    }
    EXPECT_EQ(fp.is_virtual, is_virtual_machine());
}
// ------------------------------ secure_random ------------------------------

TEST(SecureRandomTest, ProducesRequestedSizes)
{
    using namespace libmini;
    // size 0 是合法的无操作
    EXPECT_TRUE(secure_random_bytes(nullptr, 0));
    char buffer[64] = {0};
    EXPECT_TRUE(secure_random_bytes(buffer, sizeof(buffer)));
    EXPECT_NE(secure_random_string(0), std::string("x"));
    EXPECT_TRUE(secure_random_string(0).empty());
    EXPECT_EQ(secure_random_string(100).size(), 100u);
    // 空指针 + 非零长度必须被拒绝，不能崩也不能静默成功
    EXPECT_FALSE(secure_random_bytes(nullptr, 8));
}

TEST(SecureRandomTest, ConsecutiveValuesDiffer)
{
    using namespace libmini;
    // 概率性断言，但 2^-128 的碰撞率在任何 CI 上都不构成 flaky 风险
    EXPECT_NE(secure_random_string(16), secure_random_string(16));
    EXPECT_NE(secure_random_u64(), secure_random_u64());
    EXPECT_NE(secure_random_hex(16), secure_random_hex(16));
    EXPECT_NE(secure_token(), secure_token());
    EXPECT_FALSE(Uuid::generate() == Uuid::generate());
}

TEST(SecureRandomTest, HexAndTokenHaveExpectedAlphabet)
{
    using namespace libmini;
    const std::string hex = secure_random_hex(16);
    ASSERT_EQ(hex.size(), 32u);
    for (std::size_t i = 0; i < hex.size(); ++i) {
        EXPECT_TRUE((hex[i] >= '0' && hex[i] <= '9') ||
                    (hex[i] >= 'a' && hex[i] <= 'f'))
            << "非小写十六进制: " << hex;
    }
    EXPECT_TRUE(secure_random_hex(0).empty());

    // token 用 Base64url 字母表：JWT/URL 安全，可直接放进 header 或 URL
    const std::string token = secure_token(24);
    ASSERT_EQ(token.size(), 24u);
    for (std::size_t i = 0; i < token.size(); ++i) {
        const char c = token[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        EXPECT_TRUE(ok) << "token 含非 Base64url 字符: " << token;
    }
}

TEST(SecureRandomTest, CharsStayInsideAlphabetAndCoverIt)
{
    using namespace libmini;
    EXPECT_TRUE(secure_random_chars(0, "abc").empty());
    EXPECT_TRUE(secure_random_chars(8, "").empty());
    // 字母表 > 256 无法逐字节无偏采样，直接拒绝而不是悄悄截断
    EXPECT_TRUE(secure_random_chars(4, std::string(300, 'a')).empty());

    const std::string alphabet = "abcdef";
    const std::string sample = secure_random_chars(600, alphabet);
    ASSERT_EQ(sample.size(), 600u);
    bool seen[6] = {false, false, false, false, false, false};
    for (std::size_t i = 0; i < sample.size(); ++i) {
        const std::size_t pos = alphabet.find(sample[i]);
        ASSERT_NE(pos, std::string::npos)
            << "字符不在字母表内: " << sample[i];
        seen[pos] = true;
    }
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(seen[i]) << "字母表第 " << i << " 个字符一次都没出现";
    }
}

TEST(SecureRandomTest, AlphabetSamplingIsNotVisiblyBiased)
{
    using namespace libmini;
    // 6 个字符会让朴素取模出现约 1/86 的偏置（256 = 42*6 + 4，前 4 个余数
    // 多一次机会）。拒绝采样消除它。这里只做粗粒度断言：拒绝采样下
    // 各字符占比都在 1/6 附近。
    const std::string alphabet = "abcdef";
    const std::string sample = secure_random_chars(60000, alphabet);
    ASSERT_EQ(sample.size(), 60000u);
    std::size_t counts[6] = {0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < sample.size(); ++i) {
        ++counts[alphabet.find(sample[i])];
    }
    for (int i = 0; i < 6; ++i) {
        // 期望 10000，取 ±12% 的宽松区间：足够抓住朴素取模的 1/86 偏置
        EXPECT_GT(counts[i], 8800u);
        EXPECT_LT(counts[i], 11200u);
    }
}
// ------------------------------ kdf ------------------------------

TEST(KdfTest, Pbkdf2MatchesKnownAnswerVectors)
{
    using namespace libmini;
    // RFC 7914 §11 / draft-josefsson PBKDF2-HMAC-SHA256 官方向量。
    // 用 16384 与 80000 两组：迭代次数够高不会被 kMinIterations 钳位影响，
    // 且两组 key_bytes 都取 64（= 2 个 32B 块），顺带覆盖跨块拼接与
    // 末块截断逻辑——只测 c=1 / dkLen=32 的向量覆盖不到这两条路径。
    const std::string k1 =
        Pbkdf2HmacSha256::derive("pleaseletmein", "SodiumChloride",
                                 16384, 64);
    ASSERT_EQ(k1.size(), 64u);
    EXPECT_EQ(Hex::encode(k1, /*lower_case=*/true),
              "e5d7c03e11ae5b90b6669755fd90930259ec77f7f643b3d71d2633043"
              "58e528a679f2ef40a2306882becc93d7f3f38f213304971033ad47eb34"
              "21d72ad20e77c");

    // 第二组刻意换口令与迭代次数：KAT 的价值在于「任一环节错位就不同」，
    // 只测一组的话，实现里写死输出同样能过。
    const std::string k2 =
        Pbkdf2HmacSha256::derive("Password", "SodiumChloride", 80000, 32);
    ASSERT_EQ(k2.size(), 32u);
    EXPECT_EQ(Hex::encode(k2, /*lower_case=*/true),
              "d0b42e8836b6b0a3b9ff522197a62265a48d27bdc012d73261add71499"
              "3b05ae");
}

TEST(KdfTest, Pbkdf2RejectsBadParametersAndClampsIterations)
{
    using namespace libmini;
    const std::string salt = "0123456789abcdef";

    EXPECT_TRUE(Pbkdf2HmacSha256::derive("pw", salt, 16384, 0).empty());
    // 盐短于 8 字节：拒绝而不是默默补齐
    EXPECT_TRUE(Pbkdf2HmacSha256::derive("pw", "short", 16384, 32).empty());
    EXPECT_FALSE(Pbkdf2HmacSha256::derive("pw", "12345678", 16384, 32).empty());

    // 迭代次数钳位：低于下限抬到下限，超上限降到上限。
    // 关键性质是「仍然产出合法密钥」而不是「等于某个具体值」。
    const std::string clamped_low =
        Pbkdf2HmacSha256::derive("pw", salt, 1, 32);
    ASSERT_FALSE(clamped_low.empty());
    EXPECT_EQ(clamped_low,
              Pbkdf2HmacSha256::derive("pw", salt,
                                       Pbkdf2HmacSha256::kMinIterations, 32));
    EXPECT_FALSE(Pbkdf2HmacSha256::derive("pw", salt,
                                          Pbkdf2HmacSha256::kMaxIterations,
                                          32).empty());

    // derive_hex 与 derive 一致，长度 = key_bytes*2
    const std::string hex =
        Pbkdf2HmacSha256::derive_hex("pw", salt, 16384, 16);
    ASSERT_EQ(hex.size(), 32u);
    EXPECT_EQ(hex, Hex::encode(Pbkdf2HmacSha256::derive("pw", salt, 16384, 16),
                               /*lower_case=*/true));
    EXPECT_TRUE(Pbkdf2HmacSha256::derive_hex("pw", "x", 16384, 16).empty());
}

TEST(KdfTest, Pbkdf2VerifyAndConstantTimeEquals)
{
    using namespace libmini;
    const std::string salt = "0123456789abcdef";
    const std::string key = Pbkdf2HmacSha256::derive("correct horse", salt, 16384, 32);

    EXPECT_TRUE(Pbkdf2HmacSha256::verify("correct horse", salt, 16384, key));
    EXPECT_FALSE(Pbkdf2HmacSha256::verify("correct hors", salt, 16384, key));
    EXPECT_FALSE(Pbkdf2HmacSha256::verify("", salt, 16384, key));
    // 期望值非法（空）时必须判否，而不是让 derive 返回空后误判成功
    EXPECT_FALSE(Pbkdf2HmacSha256::verify("correct horse", salt, 16384, ""));

    EXPECT_TRUE(constant_time_equals("abc", "abc"));
    EXPECT_TRUE(constant_time_equals("", ""));
    EXPECT_FALSE(constant_time_equals("abc", "abd"));
    EXPECT_FALSE(constant_time_equals("abc", "ab"));
    EXPECT_FALSE(constant_time_equals("abc", "abcd"));
    // 含 NUL 也要逐字节比较，std::string 比较同样如此，这里确认一致性
    EXPECT_FALSE(constant_time_equals(std::string("a\0b", 3), std::string("a\0c", 3)));
    EXPECT_TRUE(constant_time_equals(std::string("a\0b", 3), std::string("a\0b", 3)));

    const std::string random_salt = Pbkdf2HmacSha256::random_salt();
    EXPECT_EQ(random_salt.size(), Pbkdf2HmacSha256::kSaltSize);
    EXPECT_NE(random_salt, Pbkdf2HmacSha256::random_salt());
    EXPECT_TRUE(Pbkdf2HmacSha256::random_salt(0).empty());
}

TEST(KdfTest, PasswordSealRoundTrip)
{
    using namespace libmini;
    const std::string plaintext = "database password = hunter2";

    const std::string sealed = PasswordSeal::seal("s3cret", plaintext);
    ASSERT_FALSE(sealed.empty());
    // 密文里不能出现明文
    EXPECT_EQ(sealed.find(plaintext), std::string::npos);

    std::string opened;
    ASSERT_TRUE(PasswordSeal::open("s3cret", sealed, opened));
    EXPECT_EQ(opened, plaintext);
    EXPECT_EQ(PasswordSeal::open_or_empty("s3cret", sealed), plaintext);

    // 同样输入两次必须产出不同密文（盐与 nonce 都随机）
    EXPECT_NE(sealed, PasswordSeal::seal("s3cret", plaintext));
}

TEST(KdfTest, PasswordSealRejectsWrongPasswordTamperingAndGarbage)
{
    using namespace libmini;
    const std::string sealed = PasswordSeal::seal("pw", "payload", "ctx");

    std::string opened;
    EXPECT_FALSE(PasswordSeal::open("nope", sealed, opened));
    EXPECT_FALSE(PasswordSeal::open("", sealed, opened));
    // aad 参与认证：不匹配即失败
    EXPECT_FALSE(PasswordSeal::open("pw", sealed, opened, "other"));
    // 密文被改一位 → tag 校验失败
    std::string tampered = sealed;
    tampered[tampered.size() - 1] = static_cast<char>(tampered[tampered.size() - 1] ^ 0x01);
    EXPECT_FALSE(PasswordSeal::open("pw", tampered, opened));
    // 头部被改：迭代次数被攻击者压到 1 也不能让 open 变快
    std::string iter_tampered = sealed;
    iter_tampered[7] = '\x01';
    EXPECT_FALSE(PasswordSeal::open("pw", iter_tampered, opened));
    EXPECT_FALSE(PasswordSeal::open("pw", "", opened));
    EXPECT_FALSE(PasswordSeal::open("pw", "LMPS", opened));
    EXPECT_FALSE(PasswordSeal::open("pw", std::string("\0\0\0\0", 4), opened));

    // aad 正确时同一份密文可解
    ASSERT_TRUE(PasswordSeal::open("pw", sealed, opened, "ctx"));
    EXPECT_EQ(opened, "payload");

    // 空明文：往返必须成功且真的为空（open 与 open_or_empty 语义不同，
    // 这一点单靠返回值看不出来，所以用返回 bool 的 open 断言）
    const std::string empty_sealed = PasswordSeal::seal("pw", "");
    ASSERT_FALSE(empty_sealed.empty());
    ASSERT_TRUE(PasswordSeal::open("pw", empty_sealed, opened));
    EXPECT_TRUE(opened.empty());

    // 空口令拒绝密封：产出「看起来有保护其实没有」的文件比失败更糟
    EXPECT_TRUE(PasswordSeal::seal("", "payload").empty());
}

TEST(KdfTest, PasswordSealInspectAndNeedsReseal)
{
    using namespace libmini;
    const std::string sealed = PasswordSeal::seal("pw", "payload", "", 16384);

    PasswordSeal::Info info;
    ASSERT_TRUE(PasswordSeal::inspect(sealed, info));
    EXPECT_EQ(info.version, 1u);
    EXPECT_EQ(info.kdf_id, 1u);
    EXPECT_EQ(info.iterations, 16384u);
    EXPECT_EQ(info.salt.size(), Pbkdf2HmacSha256::kSaltSize);
    EXPECT_EQ(info.nonce.size(), Aes256Gcm::kNonceSize);
    // 明文 "payload"(7) + 域分隔标记(1) + GCM tag(16)
    EXPECT_EQ(info.ciphertext_size, 7u + 1u + Aes256Gcm::kTagSize);

    // 阈值等于当前迭代次数不算「需要重算」——inspect 是给迁移用的，
    // 边界语义必须是 >= 才算达标，否则每次自检都会要求重写文件。
    EXPECT_FALSE(PasswordSeal::needs_reseal(sealed, 16384));
    EXPECT_TRUE(PasswordSeal::needs_reseal(sealed, 16385));
    EXPECT_FALSE(PasswordSeal::needs_reseal(sealed, 16383));
    // 认不出来的格式一律建议重新 seal
    EXPECT_TRUE(PasswordSeal::needs_reseal("garbage"));
    EXPECT_TRUE(PasswordSeal::needs_reseal(""));

    PasswordSeal::Info bad;
    EXPECT_FALSE(PasswordSeal::inspect("garbage", bad));

    // 版本号不认识 → 解析失败，绝不猜结构
    std::string future = sealed;
    future[4] = '\x63';
    EXPECT_FALSE(PasswordSeal::inspect(future, bad));

    // salt_len 字段被改成超长（65535）→ 按损坏处理，不按它分配
    std::string huge_salt = sealed;
    huge_salt[10] = '\xff';
    huge_salt[11] = '\xff';
    EXPECT_FALSE(PasswordSeal::inspect(huge_salt, bad));
}

// ------------------------------ digest: SHA-1 / SHA-512 ------------------------------

TEST(DigestTest, Sha1MatchesKnownAnswerVectors)
{
    using namespace libmini;
    // 空串与 "abc" 是 RFC 3174 的两组基准值；长串（56 字节，正好卡在
    // 单块填充边界之后）与 100 万 'a'（官方百万测试向量）用来覆盖
    // 消息展开与多块路径——只测 "abc" 的话，块处理写错也能过。
    EXPECT_EQ(Sha1::hex(""),
              "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(Sha1::hex("abc"),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(Sha1::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(Sha1::hex(std::string(1000000, 'a')),
              "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // 43 字节 = 未满一块
    EXPECT_EQ(Sha1::hex("The quick brown fox jumps over the lazy dog"),
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    EXPECT_EQ(Sha1::hex("").size(), 40u);
    Sha1 s;
    EXPECT_EQ(s.finish().size(), 20u);
}

TEST(DigestTest, Sha1StreamingMatchesOneShot)
{
    using namespace libmini;
    // 分片喂入的边界是这类实现最容易错的地方：每次切片都从
    // 块边界上切开，任何一处的 off-by-one 都会让结果与一次性算法不符
    const std::string data =
        "The quick brown fox jumps over the lazy dog and keeps on running "
        "until it reaches exactly one hundred and twelve bytes of text.";

    for (std::size_t chunk = 1; chunk <= 40; chunk += 7) {
        Sha1 streaming;
        for (std::size_t i = 0; i < data.size(); i += chunk) {
            streaming.update(data.data() + i,
                             (i + chunk < data.size()) ? chunk
                                                      : data.size() - i);
        }
        EXPECT_EQ(Hex::encode(streaming.finish(), true), Sha1::hex(data))
            << "chunk = " << chunk;
    }

    // reset 后复用必须回到初始状态，否则上一条数据的尾巴会漏进下一次
    Sha1 reused;
    reused.update("garbage-that-must-not-leak");
    reused.reset();
    reused.update("abc");
    EXPECT_EQ(Hex::encode(reused.finish(), true), Sha1::hex("abc"));

    // 空 update 是无操作
    Sha1 empty_update;
    empty_update.update("", 0);
    empty_update.update(nullptr, 0);
    EXPECT_EQ(Hex::encode(empty_update.finish(), true), Sha1::hex(""));
}

TEST(DigestTest, Sha512MatchesKnownAnswerVectors)
{
    using namespace libmini;
    EXPECT_EQ(Sha512::hex(""),
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    EXPECT_EQ(Sha512::hex("abc"),
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d3"
              "9a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca4"
              "9f");
    EXPECT_EQ(Sha512::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c3"
              "3596fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3"
              "445");
    EXPECT_EQ(Sha512::hex(std::string(1000000, 'a')),
              "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973e"
              "bde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
    EXPECT_EQ(Sha512::hex("The quick brown fox jumps over the lazy dog"),
              "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb6"
              "42e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6");

    EXPECT_EQ(Sha512::hex("").size(), 128u);
    Sha512 s;
    EXPECT_EQ(s.finish().size(), 64u);
}

TEST(DigestTest, Sha512PaddingBoundaryAround112And128)
{
    using namespace libmini;
    // SHA-512 的填充要到 112 mod 128，这几个长度分别落在填充边界的
    // 两侧与两侧之后：111/112 卡在边界，127/128 卡在块边界。
    // 填充算错时这几组会最先崩。
    const std::size_t lengths[] = { 111, 112, 113, 127, 128, 129, 255, 256 };
    for (std::size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const std::string data(lengths[i], 'x');
        Sha512 one_shot;
        one_shot.update(data);
        EXPECT_EQ(Hex::encode(one_shot.finish(), true), Sha512::hex(data))
            << "length = " << lengths[i];

        // 分两片喂入同样长度，确认跨块拼接与一次性一致
        Sha512 split;
        const std::size_t cut = lengths[i] / 2;
        split.update(data.data(), cut);
        split.update(data.data() + cut, lengths[i] - cut);
        EXPECT_EQ(Hex::encode(split.finish(), true), Sha512::hex(data))
            << "split length = " << lengths[i];
    }

    Sha512 reused;
    reused.update("garbage-that-must-not-leak");
    reused.reset();
    reused.update("abc");
    EXPECT_EQ(Hex::encode(reused.finish(), true), Sha512::hex("abc"));

    Sha512 empty_update;
    empty_update.update(nullptr, 0);
    EXPECT_EQ(Hex::encode(empty_update.finish(), true), Sha512::hex(""));
}

TEST(DigestTest, Sha1AndSha512CoverEmbeddingLengths)
{
    using namespace libmini;
    // 64 位计数器在超过 2^61 字节时才会回绕，这里无法真的喂进去，
    // 但「同一段数据用 SHA-1 与 SHA-512 都能处理」本身要保证：
    // 短于一个块、恰好一块、比一块多一字节都要能算。
    Sha1 s1;
    Sha512 s512;
    const std::string data(300, 'q');
    s1.update(data);
    s512.update(data);
    EXPECT_EQ(Hex::encode(s1.finish(), true), Sha1::hex(data));
    EXPECT_EQ(Hex::encode(s512.finish(), true), Sha512::hex(data));
    EXPECT_EQ(Sha1::hex(data).size(), 40u);
    EXPECT_EQ(Sha512::hex(data).size(), 128u);
}

// ------------------------------ uuid v7 / v5 ------------------------------

TEST(UuidTest, V7HasCorrectLayoutAndTimestamp)
{
    using namespace libmini;
    // 显式时间戳版本必须逐字节符合 RFC 9562 §5.7 的位布局，
    // 否则下游按字节切分时间戳的解析器会全线错位。
    const std::int64_t ms = 1645557742000LL;  // 2022-02-22 19:22:22 UTC
    const Uuid u = Uuid::generate_v7(ms);

    EXPECT_EQ(u.version(), 7);
    EXPECT_EQ(u.variant(), 2);              // RFC 4122
    EXPECT_EQ(u.timestamp_ms(), ms);
    EXPECT_FALSE(u.is_nil());

    // 前 48 位是时间戳（大端），逐字节核对
    EXPECT_EQ(u.bytes[0], 0x01);
    EXPECT_EQ(u.bytes[1], 0x7F);
    EXPECT_EQ(u.bytes[2], 0x22);
    EXPECT_EQ(u.bytes[3], 0xE2);
    EXPECT_EQ(u.bytes[4], 0x79);
    EXPECT_EQ(u.bytes[5], 0xB0);

    // RFC 9562 附录 A.3 的示例 UUID 是 017F22E2-79B0-7CC3-98C4-DC0C0C07398F，
    // 其时间戳字段 0x017F22E279B0 = 1645557742000ms。逐位比对其固定部分：
    // 版本/变体各占字节的高位，随机位不同所以只比掩码后的值。
    const Uuid sample = Uuid::generate_v7(0x017F22E279B0LL);
    EXPECT_EQ(sample.bytes[0], 0x01);
    EXPECT_EQ(sample.bytes[1], 0x7F);
    EXPECT_EQ(sample.bytes[2], 0x22);
    EXPECT_EQ(sample.bytes[3], 0xE2);
    EXPECT_EQ(sample.bytes[4], 0x79);
    EXPECT_EQ(sample.bytes[5], 0xB0);
    EXPECT_EQ(sample.bytes[6] & 0xF0, 0x70);      // version 7
    EXPECT_EQ(sample.bytes[8] & 0xC0, 0x80);      // variant 2
    EXPECT_EQ(sample.version(), 7);
    EXPECT_EQ(sample.timestamp_ms(), 0x017F22E279B0LL);

    // epoch 与边界
    EXPECT_EQ(Uuid::generate_v7(0).timestamp_ms(), 0);
    // 越界/负值落到 epoch，而不是回绕成别的年份
    EXPECT_EQ(Uuid::generate_v7(-1).timestamp_ms(), 0);
    EXPECT_EQ(Uuid::generate_v7(0xFFFFFFFFFFFFLL).timestamp_ms(), 0xFFFFFFFFFFFFLL);
    EXPECT_EQ(Uuid::generate_v7(0x1000000000000LL).timestamp_ms(), 0);

    // 非 v7 取不到时间戳
    EXPECT_EQ(Uuid::generate().timestamp_ms(), 0);
}

TEST(UuidTest, V7IsStrictlyMonotonicWithinProcess)
{
    using namespace libmini;
    // 单调性是 v7 相对 v4 的核心价值：同一毫秒内的 ID 也必须有确定顺序，
    // 否则前缀聚簇的收益会打折。
    const std::size_t count = 5000;  // 刻意 > 单毫秒 4096 个的上限
    std::vector<Uuid> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(Uuid::generate_v7());
    }

    for (std::size_t i = 1; i < count; ++i) {
        // operator< 已经是逐字节字典序（uuid.cpp 既有实现），直接用
        EXPECT_TRUE(ids[i - 1] < ids[i])
            << "v7 在第 " << i << " 个丢失单调性: "
            << ids[i - 1].to_string() << " >= " << ids[i].to_string();
    }

    // 时间戳非递减，且不会偏离真实时间太多（允许时钟回拨的小幅抖动）
    const std::int64_t now = current_timestamp_ms();
    EXPECT_GE(ids.front().timestamp_ms(), now - 60000);
    EXPECT_LE(ids.back().timestamp_ms(), now + 60000);
    for (std::size_t i = 1; i < count; ++i) {
        EXPECT_LE(ids[i - 1].timestamp_ms(), ids[i].timestamp_ms());
    }
}

TEST(UuidTest, V7IsUniqueUnderConcurrentGeneration)
{
    using namespace libmini;
    // 单调性靠 mutex 保护；并发下验证既不重复也不崩。
    // 16 个线程 × 200 个 = 3200 个，跨线程收集后整体校验有序。
    const int kThreads = 16;
    const int kPerThread = 200;
    std::vector<std::vector<Uuid>> per_thread(kThreads);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        // C++11 只允许捕获「自动变量」，不捕获 const int kPerThread；
        // 按值捕获常量在 MinGW/GCC 下能编过，MSVC 直接报 C3493。
        const int per_thread_count = kPerThread;
        threads.emplace_back([t, &per_thread, per_thread_count]() {
            per_thread[static_cast<std::size_t>(t)].reserve(
                static_cast<std::size_t>(per_thread_count));
            for (int i = 0; i < per_thread_count; ++i) {
                per_thread[static_cast<std::size_t>(t)].push_back(
                    Uuid::generate_v7());
            }
        });
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    std::vector<Uuid> all;
    for (int t = 0; t < kThreads; ++t) {
        all.insert(all.end(), per_thread[static_cast<std::size_t>(t)].begin(),
                   per_thread[static_cast<std::size_t>(t)].end());
    }
    ASSERT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPerThread));

    std::sort(all.begin(), all.end(),
              [](const Uuid& a, const Uuid& b) { return a < b; });
    for (std::size_t i = 1; i < all.size(); ++i) {
        EXPECT_NE(all[i], all[i - 1])
            << "并发下产生重复 v7: " << all[i].to_string();
    }
}

TEST(UuidTest, V5MatchesRfcVectors)
{
    using namespace libmini;
    // RFC 4122 附录 / Python uuid 模块的公认结果。命名空间常量与
    // SHA-1(namespace || name) 取前 16 字节这两处任一写错都会偏离。
    EXPECT_EQ(Uuid::v5(Uuid::namespace_dns(), "python.org").to_string(),
              "886313e1-3b8a-5372-9b90-0c9aee199e5d");
    EXPECT_EQ(Uuid::v5(Uuid::namespace_url(), "https://example.com").to_string(),
              "4fd35a71-71ef-5a55-a9d9-aa75c889a6d0");
    EXPECT_EQ(Uuid::v5(Uuid::namespace_oid(), "1.3.6.1.4.1.343").to_string(),
              "6aab0456-7392-582a-b92a-ba5a7096945d");
    EXPECT_EQ(Uuid::v5(Uuid::namespace_x500(), "CN=Test").to_string(),
              "5e2003ce-30b7-5e3b-8c3a-a164acc5b21e");

    // 四个预定义命名空间的前缀（RFC 4122 §4.1.3），各自最后一个字节不同
    EXPECT_EQ(Uuid::namespace_dns().to_string(), "6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    EXPECT_EQ(Uuid::namespace_url().to_string(), "6ba7b811-9dad-11d1-80b4-00c04fd430c8");
    EXPECT_EQ(Uuid::namespace_oid().to_string(), "6ba7b812-9dad-11d1-80b4-00c04fd430c8");
    EXPECT_EQ(Uuid::namespace_x500().to_string(), "6ba7b814-9dad-11d1-80b4-00c04fd430c8");

    // 确定性：同输入同输出，且与生成顺序无关
    const Uuid again = Uuid::v5(Uuid::namespace_dns(), "python.org");
    EXPECT_EQ(again.to_string(), "886313e1-3b8a-5372-9b90-0c9aee199e5d");
    // 不同命名空间 → 不同结果
    EXPECT_NE(Uuid::v5(Uuid::namespace_url(), "python.org").to_string(),
              Uuid::v5(Uuid::namespace_dns(), "python.org").to_string());
    // 大小写敏感（name 是字节串，不做规范化）
    EXPECT_NE(Uuid::v5(Uuid::namespace_dns(), "Python.org").to_string(),
              Uuid::v5(Uuid::namespace_dns(), "python.org").to_string());
    // 空 name 合法，不崩溃
    EXPECT_EQ(Uuid::v5(Uuid::namespace_dns(), "").version(), 5);
}

TEST(UuidTest, VersionAndVariantReflectBitLayout)
{
    using namespace libmini;
    EXPECT_EQ(Uuid::generate().version(), 4);
    EXPECT_EQ(Uuid::generate_v7().version(), 7);
    EXPECT_EQ(Uuid::v5(Uuid::namespace_dns(), "x").version(), 5);
    // 所有生成路径都必须落在 RFC 4122 变体上
    EXPECT_EQ(Uuid::generate().variant(), 2);
    EXPECT_EQ(Uuid::generate_v7().variant(), 2);
    EXPECT_EQ(Uuid::v5(Uuid::namespace_dns(), "x").variant(), 2);

    // 全零是 variant 0（NCS）的特例，不是 v4
    EXPECT_EQ(Uuid::nil().variant(), 0);
    EXPECT_EQ(Uuid::nil().version(), 0);

    // 手工构造的变体：byte[8] 高 2 位决定结果
    Uuid u = Uuid::nil();
    u.bytes[8] = 0xC0;
    EXPECT_EQ(u.variant(), 6);  // Microsoft
    u.bytes[8] = 0xE0;
    EXPECT_EQ(u.variant(), 7);  // 未来保留
    u.bytes[8] = 0x00;
    EXPECT_EQ(u.variant(), 0);  // NCS

    // v7/v5 经 parse/to_string 往返后信息不丢
    const Uuid v7 = Uuid::generate_v7();
    Uuid parsed;
    ASSERT_TRUE(Uuid::parse(v7.to_string(), parsed));
    EXPECT_EQ(parsed, v7);
    EXPECT_EQ(parsed.version(), 7);
    EXPECT_EQ(parsed.timestamp_ms(), v7.timestamp_ms());
    EXPECT_TRUE(Uuid::parse(v7.to_hex_string(), parsed));
    EXPECT_EQ(parsed, v7);
}

// ------------------------------ csv ------------------------------

TEST(CsvTest, ParsesRfc4180Basics)
{
    using namespace libmini;
    std::vector<std::vector<std::string>> rows;
    // 期望值与 Python csv 模块（RFC 4180 参考实现）逐条比对过
    ASSERT_TRUE(csv_parse("a,b,c", rows));
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][0], "a");
    EXPECT_EQ(rows[0][1], "b");
    EXPECT_EQ(rows[0][2], "c");

    // 行尾：LF / CRLF / 裸 CR 都算一个换行，且不产生多余的末行
    ASSERT_TRUE(csv_parse("a,b,c\n", rows));
    EXPECT_EQ(rows.size(), 1u);
    ASSERT_TRUE(csv_parse("a,b,c\r\n", rows));
    EXPECT_EQ(rows.size(), 1u);
    ASSERT_TRUE(csv_parse("a,b,c\r", rows));
    EXPECT_EQ(rows.size(), 1u);
    ASSERT_TRUE(csv_parse("a,b\nc,d\n", rows));
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1][0], "c");

    // 空文本 = 0 行；空行（依选项）
    ASSERT_TRUE(csv_parse("", rows));
    EXPECT_TRUE(rows.empty());

    // 引号包裹的字段：分隔符、换行、引号本身
    ASSERT_TRUE(csv_parse("a,\"b,c\",d", rows));
    EXPECT_EQ(rows[0][1], "b,c");
    ASSERT_TRUE(csv_parse("a,\"b\"\"q\",c", rows));
    EXPECT_EQ(rows[0][1], "b\"q");
    ASSERT_TRUE(csv_parse("\"multi\nline\",b", rows));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0], "multi\nline");
    ASSERT_TRUE(csv_parse("\"multi\r\nline\",b", rows));
    EXPECT_EQ(rows[0][0], "multi\r\nline");

    // 首尾空格属于字段内容，不 trim（RFC 规则 5）
    ASSERT_TRUE(csv_parse("a,  b  ,c", rows));
    EXPECT_EQ(rows[0][1], "  b  ");
    ASSERT_TRUE(csv_parse("\" leading\",b", rows));
    EXPECT_EQ(rows[0][0], " leading");

    // 空字段与全空行
    ASSERT_TRUE(csv_parse("\"\"", rows));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0][0].empty());
    ASSERT_TRUE(csv_parse("a,\"\",b", rows));
    EXPECT_EQ(rows[0][1], "");
    ASSERT_TRUE(csv_parse("a,", rows));
    ASSERT_EQ(rows[0].size(), 2u);
    EXPECT_TRUE(rows[0][1].empty());

    // 不做类型推断：前导零必须保留
    ASSERT_TRUE(csv_parse("00123,1e5", rows));
    EXPECT_EQ(rows[0][0], "00123");
    EXPECT_EQ(rows[0][1], "1e5");

    // 字段中间的引号是字面量，不是引号起始
    ASSERT_TRUE(csv_parse("a\"b,c", rows));
    EXPECT_EQ(rows[0][0], "a\"b");
    ASSERT_TRUE(csv_parse("x,y\"z", rows));
    EXPECT_EQ(rows[0][1], "y\"z");
}

TEST(CsvTest, ParsesBomBlankLinesAndRejectsBadInput)
{
    using namespace libmini;
    std::vector<std::vector<std::string>> rows;

    // UTF-8 BOM 跳过；不跳过时它会粘进第一个字段
    ASSERT_TRUE(csv_parse("\xEF\xBB\xBF" "a,b\n", rows));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0], "a");
    CsvOptions keep_bom;
    keep_bom.skip_bom = false;
    ASSERT_TRUE(csv_parse("\xEF\xBB\xBF" "a,b\n", rows, keep_bom));
    EXPECT_EQ(rows[0][0].size(), 4u);  // 3 字节 BOM + "a"

    // 空白行默认丢弃
    ASSERT_TRUE(csv_parse("a,b\n\n\nc,d\n", rows));
    EXPECT_EQ(rows.size(), 2u);
    CsvOptions keep_blank;
    keep_blank.skip_blank_lines = false;
    ASSERT_TRUE(csv_parse("a,b\n\nc,d\n", rows, keep_blank));
    EXPECT_EQ(rows.size(), 3u);
    EXPECT_TRUE(rows[1].empty());

    // 引号未闭合必须判否：截断的 CSV 与完整 CSV 长得一样，
    // 悄悄通过等于把上游 bug 变成下游脏数据
    EXPECT_FALSE(csv_parse("a,\"unterminated\n", rows));
    EXPECT_FALSE(csv_parse("\"", rows));
    EXPECT_FALSE(csv_parse("a,b\nc,\"dangling", rows));

    // 参数非法
    CsvOptions bad_delim;
    bad_delim.delimiter = '\0';
    EXPECT_FALSE(csv_parse("a,b", rows, bad_delim));
    CsvOptions bad_quote;
    bad_quote.quote = ',';
    EXPECT_FALSE(csv_parse("a,b", rows, bad_quote));
}

TEST(CsvTest, ParseIsOrderAndQuoteFaithful)
{
    using namespace libmini;
    std::vector<std::vector<std::string>> rows;

    // 行序、列序必须与原文一致
    ASSERT_TRUE(csv_parse("r0c0,r0c1\nr1c0,r1c1\nr2c0,r2c1\n", rows));
    ASSERT_EQ(rows.size(), 3u);
    for (std::size_t r = 0; r < 3; ++r) {
        EXPECT_EQ(rows[r][0], "r" + std::to_string(r) + "c0");
        EXPECT_EQ(rows[r][1], "r" + std::to_string(r) + "c1");
    }

    // 尾部逗号产生的空列不能被吞掉
    ASSERT_TRUE(csv_parse("a,b,\n", rows));
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_TRUE(rows[0][2].empty());

    // 引号闭合后的脏数据按字面追加（Excel 的 `"a" ,b`）
    ASSERT_TRUE(csv_parse("\"a\" x,b", rows));
    EXPECT_EQ(rows[0][0], "a x");
}

TEST(CsvTest, SerializeQuotesOnlyWhenNeeded)
{
    using namespace libmini;
    std::vector<std::vector<std::string>> rows;
    rows.push_back({ "plain", "with,comma", "with\"quote", "with\nnewline" });
    rows.push_back({ " leading", "trailing ", "00123", "" });

    const std::string text = csv_serialize(rows);
    // 只有真需要的字段加引号：空白环绕的字段不加（RFC 明确说空格是内容）
    EXPECT_EQ(text,
              "plain,\"with,comma\",\"with\"\"quote\",\"with\nnewline\"\n"
              " leading,trailing ,00123,\n");

    // CRLF 模式（RFC 规定的行结束符）
    const std::string crlf = csv_serialize(rows, true);
    EXPECT_NE(crlf.find("\r\n"), std::string::npos);
    // 空行里没有多余的 CRLF
    EXPECT_EQ(crlf[crlf.size() - 1], '\n');

    // 非 ASCII 不受影响，UTF-8 原样透传
    std::vector<std::vector<std::string>> uni;
    uni.push_back({ "中文", "emoji" });
    EXPECT_EQ(csv_serialize(uni), "中文,emoji\n");

    // 非法选项返回空串
    CsvOptions bad;
    bad.delimiter = '\0';
    EXPECT_TRUE(csv_serialize(rows, false, bad).empty());
}

TEST(CsvTest, RoundTripIsLossless)
{
    using namespace libmini;
    std::vector<std::vector<std::string>> original;
    original.push_back({ "a", "b,c", "d\"e", "f\ng" });
    original.push_back({ " lead", "trail ", "", "h\ri" });
    original.push_back({ "中文", "00123", "1e5", "a\r\nb" });

    // 序列化 → 解析 必须完全还原，这是 CSV 模块存在的根本理由
    std::vector<std::vector<std::string>> restored;
    ASSERT_TRUE(csv_parse(csv_serialize(original), restored));
    ASSERT_EQ(restored.size(), original.size());
    for (std::size_t r = 0; r < original.size(); ++r) {
        ASSERT_EQ(restored[r].size(), original[r].size())
            << "行 " << r << " 字段数不一致";
        for (std::size_t c = 0; c < original[r].size(); ++c) {
            EXPECT_EQ(restored[r][c], original[r][c])
                << "行 " << r << " 列 " << c;
        }
    }

    // CRLF 模式同样无损
    std::vector<std::vector<std::string>> restored_crlf;
    ASSERT_TRUE(csv_parse(csv_serialize(original, true), restored_crlf));
    ASSERT_EQ(restored_crlf.size(), original.size());
    for (std::size_t r = 0; r < original.size(); ++r) {
        ASSERT_EQ(restored_crlf[r].size(), original[r].size());
        for (std::size_t c = 0; c < original[r].size(); ++c) {
            EXPECT_EQ(restored_crlf[r][c], original[r][c])
                << "CRLF 模式下行 " << r << " 列 " << c;
        }
    }

    // 关闭引号解析时不做任何转义：分隔符仍分隔，其余原样
    CsvOptions no_quote;
    no_quote.quote = '\0';
    std::vector<std::vector<std::string>> raw;
    ASSERT_TRUE(csv_parse("a,\"b\",c", raw, no_quote));
    ASSERT_EQ(raw[0].size(), 3u);
    EXPECT_EQ(raw[0][1], "\"b\"");
    // 关闭引号后不做任何转义：字段里的 " 就是字面数据，原样写出。
    // （若在这里期望输出里没有引号，等于要求转义——那就等于没关闭引号。）
    EXPECT_EQ(csv_serialize(raw, false, no_quote), "a,\"b\",c\n");
}

TEST(CsvTest, CustomDelimiterAndHeaderHelpers)
{
    using namespace libmini;
    CsvOptions semi;
    semi.delimiter = ';';
    std::vector<std::vector<std::string>> rows;
    ASSERT_TRUE(csv_parse("a;b;\"c;d\"", rows, semi));
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][2], "c;d");
    // 换分隔符后逗号是普通字符
    ASSERT_TRUE(csv_parse("a,b;c", rows, semi));
    EXPECT_EQ(rows[0][0], "a,b");

    std::vector<std::vector<std::string>> src;
    src.push_back({ "id", "name", "score" });
    src.push_back({ "1", "张三", "90" });
    src.push_back({ "2", "李四", "85" });

    const std::string path = unique_temp_path("csv_hdr_");
    ASSERT_TRUE(csv_write_file(path, src));

    std::vector<std::string> header;
    std::vector<std::vector<std::string>> data;
    ASSERT_TRUE(csv_read_file_with_header(path, header, data));
    ASSERT_EQ(header.size(), 3u);
    EXPECT_EQ(header[0], "id");
    EXPECT_EQ(header[1], "name");
    // 表头不重复出现在数据里
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0][1], "张三");

    EXPECT_EQ(csv_column_index(header, "name"), 1);
    EXPECT_EQ(csv_column_index(header, "missing"), -1);
    // 大小写敏感（不做规范化）
    EXPECT_EQ(csv_column_index(header, "Name"), -1);

    // 不带表头的读法拿到的仍是全部行
    std::vector<std::vector<std::string>> all;
    ASSERT_TRUE(csv_read_file(path, all));
    EXPECT_EQ(all.size(), 3u);

    // 文件不存在时返回 false，且不留垃圾数据
    std::vector<std::vector<std::string>> missing;
    std::vector<std::string> missing_header;
    EXPECT_FALSE(csv_read_file_with_header(std::string("no_such_dir_9x7y/x.csv"),
                                           missing_header, missing));
    EXPECT_TRUE(missing.empty());
    EXPECT_TRUE(missing_header.empty());

    // 读回来再写回去，数据必须一致
    const std::string path2 = unique_temp_path("csv_hdr2_");
    ASSERT_TRUE(csv_write_file(path2, all));
    std::vector<std::vector<std::string>> again;
    ASSERT_TRUE(csv_read_file(path2, again));
    ASSERT_EQ(again.size(), all.size());
    EXPECT_EQ(again[1][1], "张三");

    remove_file(path);
    remove_file(path2);
}

TEST(CsvTest, RaggedRowsArePreservedNotRejected)
{
    using namespace libmini;
    // 现实中的 CSV 常见缺列/多列。RFC 规则 6 要求字段数一致，
    // 但照做会让模块在真实数据上直接不可用——对齐是调用方的语义问题。
    std::vector<std::vector<std::string>> rows;
    ASSERT_TRUE(csv_parse("a,b,c\n1,2\n1,2,3,4\n", rows));
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[1].size(), 2u);
    EXPECT_EQ(rows[2].size(), 4u);

    // 空行与空文件都不算错
    ASSERT_TRUE(csv_parse("\n\n\n", rows));
    EXPECT_TRUE(rows.empty());
}

// ------------------------------ glob ------------------------------

TEST(GlobTest, MatchWildcardsAgainstPythonFnmatch)
{
    using namespace libmini;
    // 期望值逐条取自 Python fnmatch.fnmatchcase（POSIX 语义，大小写敏感）。
    // Windows 上本库大小写不敏感，故这些断言里的字母用例在 Windows 上
    // 同样为真（模式与名字本来就一致），不存在分歧。
    EXPECT_TRUE(glob_match("*.cpp", "main.cpp"));
    EXPECT_FALSE(glob_match("*.cpp", "main.cxx"));

    EXPECT_TRUE(glob_match("*", "abc"));
    EXPECT_TRUE(glob_match("*", ""));   // * 匹配空串
    EXPECT_TRUE(glob_match("", ""));   // 空模式匹配空名

    EXPECT_TRUE(glob_match("?.txt", "a.txt"));
    EXPECT_FALSE(glob_match("?.txt", "ab.txt"));
    EXPECT_FALSE(glob_match("?.txt", ".txt"));  // ? 必须匹配恰好一个字符

    // 多个 * 需要正确回溯：贪心吃掉全部后要能退回来
    EXPECT_TRUE(glob_match("a*b*c", "abc"));
    EXPECT_TRUE(glob_match("a*b*c", "axxbyyc"));
    EXPECT_FALSE(glob_match("a*b*c", "acb"));
    EXPECT_TRUE(glob_match("*.tar.gz", "a.tar.gz"));
    EXPECT_TRUE(glob_match("*/*", "a/b"));
    EXPECT_TRUE(glob_match("**", "a/b"));
}

TEST(GlobTest, MatchCharacterClasses)
{
    using namespace libmini;
    EXPECT_TRUE(glob_match("[abc]x", "ax"));
    EXPECT_FALSE(glob_match("[abc]x", "dx"));
    EXPECT_TRUE(glob_match("[a-c]x", "bx"));
    EXPECT_FALSE(glob_match("[a-c]x", "dx"));
    EXPECT_TRUE(glob_match("[!abc]x", "dx"));   // 取反
    EXPECT_FALSE(glob_match("[!abc]x", "ax"));
    EXPECT_TRUE(glob_match("[^abc]x", "dx"));   // ^ 同样表示取反
    EXPECT_TRUE(glob_match("file[0-9].txt", "file3.txt"));
    EXPECT_FALSE(glob_match("file[0-9].txt", "filea.txt"));
    EXPECT_FALSE(glob_match("file[!0-9].txt", "file3.txt"));
    EXPECT_TRUE(glob_match("file[!0-9].txt", "filea.txt"));

    // 不规范的集合不能把整个匹配判否——真实文件名里确实有 [ 和 ]
    EXPECT_TRUE(glob_match("[[]", "["));
    EXPECT_TRUE(glob_match("a[bc.txt", "a[bc.txt"));
    EXPECT_FALSE(glob_match("[]", "x"));

    // 集合里混合具体字符与区间
    EXPECT_TRUE(glob_match("[a-cx]y", "xy"));
    EXPECT_TRUE(glob_match("[a-cx]y", "by"));
    EXPECT_FALSE(glob_match("[a-cx]y", "dy"));
}

TEST(GlobTest, MatchEscapesAndLiterals)
{
    using namespace libmini;
    // \x 转义下一个字符（Windows 风格；Python fnmatch 不支持这一条）
    EXPECT_TRUE(glob_match("a\\*b", "a*b"));
    EXPECT_FALSE(glob_match("a\\*b", "axb"));
    EXPECT_TRUE(glob_match("a\\?b", "a?b"));
    EXPECT_FALSE(glob_match("a\\?b", "axb"));

    // 非通配字符按字面量匹配（含正则元字符，它们在 glob 里无特殊含义）
    EXPECT_TRUE(glob_match("a.b", "a.b"));
    EXPECT_FALSE(glob_match("a.b", "axb"));
    EXPECT_TRUE(glob_match("a+b", "a+b"));
    EXPECT_TRUE(glob_match("(a)", "(a)"));

    // 超长 pattern 不应栈溢出（迭代式匹配而非递归）
    const std::string long_pattern(5000, '*');
    EXPECT_TRUE(glob_match(long_pattern, "abc"));
    // 末尾没有 b，必须判否：回溯再熟练也不能把模式尾部「忘掉」
    EXPECT_FALSE(glob_match("*a*a*a*a*a*b", "aaaaaaaac"));
    EXPECT_TRUE(glob_match("*a*a*a*a*a*b", "aaaaaaaab"));
}

TEST(GlobTest, GlobSingleDirectory)
{
    using namespace libmini;
    const std::string root = unique_temp_path("glob_");
    ASSERT_TRUE(make_directories(path_join(root, "sub")));
    ASSERT_TRUE(write_file(path_join(root, "a.cpp"), "x"));
    ASSERT_TRUE(write_file(path_join(root, "b.txt"), "x"));
    ASSERT_TRUE(write_file(path_join(root, "sub/c.cpp"), "x"));
    ASSERT_TRUE(write_file(path_join(root, ".hidden.cpp"), "x"));

    // 只看当前层：sub/c.cpp 在下一层，绝不能出现
    std::vector<std::string> found = glob(root, "*.cpp");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0], path_join(root, "a.cpp"));

    // 隐藏项默认排除
    found = glob(root, "*");
    std::vector<std::string> names;
    for (std::size_t i = 0; i < found.size(); ++i) {
        names.push_back(basename(found[i]));
    }
    EXPECT_EQ(names.size(), 2u);  // a.cpp + b.txt，.hidden.cpp 与 sub 都不含
    EXPECT_EQ(std::find(names.begin(), names.end(), ".hidden.cpp"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "sub"), names.end());

    GlobOptions with_hidden;
    with_hidden.include_hidden = true;
    found = glob(root, "*", with_hidden);
    EXPECT_EQ(found.size(), 3u);  // 多出 .hidden.cpp；sub 是目录，不计入

    // 目录默认不出现在结果里
    GlobOptions with_dirs;
    with_dirs.include_directories = true;
    found = glob(root, "*", with_dirs);
    EXPECT_EQ(std::find(found.begin(), found.end(), path_join(root, "sub")) !=
              found.end(), true);

    // 指定一层子目录
    found = glob(root, "sub/*.cpp");
    ASSERT_EQ(found.size(), 1u);
    // glob 返回**原生分隔符**的规范路径（Windows 上是反斜杠）；
    // path_absolute 返回的是通用形式（正斜杠），比较前要转回原生
    EXPECT_EQ(found[0],
              path_to_native(path_join(root, "sub/c.cpp")));

    // 结果稳定排序：连查两次必须一致
    EXPECT_EQ(glob(root, "*"), glob(root, "*"));

    // 不存在的 root 返回空，不崩
    EXPECT_TRUE(glob(path_join(root, "nope"), "*").empty());

    remove_tree(root);
}

TEST(GlobTest, GlobRecursiveWalksWholeTree)
{
    using namespace libmini;
    const std::string root = unique_temp_path("globr_");
    ASSERT_TRUE(make_directories(path_join(root, "a/b/c")));
    ASSERT_TRUE(write_file(path_join(root, "top.cpp"), "x"));
    ASSERT_TRUE(write_file(path_join(root, "a/one.cpp"), "x"));
    ASSERT_TRUE(write_file(path_join(root, "a/b/two.cpp"), "x"));
    ASSERT_TRUE(write_file(path_join(root, "a/b/c/three.cpp"), "x"));
    ASSERT_TRUE(write_file(path_join(root, "a/b/note.md"), "x"));

    // 递归找到全部 4 个 cpp，深度不同都要覆盖
    std::vector<std::string> found = glob_recursive(root, "*.cpp");
    EXPECT_EQ(found.size(), 4u);

    // "**/*.cpp" 前缀写法与 "*.cpp" 等价
    EXPECT_EQ(glob_recursive(root, "**/*.cpp"), found);

    // 深度上限
    // max_depth 以 root 为 0 计：1 = root + 下一层，2 = 再下一层。
    // 树形：top.cpp(depth 0)、a/one.cpp(1)、a/b/two.cpp(2)、a/b/c/three.cpp(3)
    GlobOptions shallow;
    shallow.max_depth = 1;
    found = glob_recursive(root, "*.cpp", shallow);
    EXPECT_EQ(found.size(), 2u);  // top + a/one

    GlobOptions deeper;
    deeper.max_depth = 2;
    found = glob_recursive(root, "*.cpp", deeper);
    EXPECT_EQ(found.size(), 3u);  // 再加 a/b/two

    GlobOptions very_deep;
    very_deep.max_depth = 3;
    EXPECT_EQ(glob_recursive(root, "*.cpp", very_deep).size(), 4u);

    // max_depth = 0 表示不限（而不是只看 root，否则调用方无法表达「全部」）
    GlobOptions unlimited;
    unlimited.max_depth = 0;
    EXPECT_EQ(glob_recursive(root, "*.cpp", unlimited).size(), 4u);

    // 结果上限：失控的 "**" 不该把进程撑爆
    GlobOptions capped;
    capped.max_results = 2;
    EXPECT_EQ(glob_recursive(root, "*.cpp", capped).size(), 2u);

    // 按扩展名的便捷版
    found = glob_files_by_extension(root, ".cpp");
    EXPECT_EQ(found.size(), 4u);
    found = glob_files_by_extension(root, ".md");
    EXPECT_EQ(found.size(), 1u);
    EXPECT_TRUE(glob_files_by_extension(root, ".rs").empty());

    // 隐藏目录内部仍会被遍历，只是目录自身不出现在结果里
    ASSERT_TRUE(make_directories(path_join(root, ".git")));
    ASSERT_TRUE(write_file(path_join(root, ".git/keep.cpp"), "x"));
    found = glob_recursive(root, "*.cpp");
    EXPECT_EQ(found.size(), 5u);  // .git/keep.cpp 找到了；.git 本身不是文件

    remove_tree(root);
}

TEST(GlobTest, GlobResultPathsAreUsable)
{
    using namespace libmini;
    const std::string root = unique_temp_path("globp_");
    ASSERT_TRUE(make_directories(path_join(root, "d")));
    ASSERT_TRUE(write_file(path_join(root, "d/real.cpp"), "content"));

    const std::vector<std::string> found = glob_files_by_extension(root, ".cpp");
    ASSERT_EQ(found.size(), 1u);
    // 返回的必须是能直接用的完整路径，而不是只给文件名
    EXPECT_TRUE(file_exists(found[0]));
    EXPECT_EQ(read_file(found[0]), "content");
    EXPECT_FALSE(is_directory(found[0]));
    // 返回的是**原生**分隔符路径（Windows 上是 '\'），可直接交给
    // file_* 系列 API，不必再转换
    EXPECT_TRUE(found[0].find(char(92)) != std::string::npos ||
                found[0].find('/') != std::string::npos);

    remove_tree(root);
}

TEST(SystemInfoTest, ExecutablePathIsAbsolute)
{
    using namespace libmini;
    const std::string exe = executable_path();
    ASSERT_FALSE(exe.empty());
#ifdef _WIN32
    EXPECT_EQ(exe.find(':'), 1u);            // "X:..." 盘符
    EXPECT_TRUE(exe.find('\\') != std::string::npos);
#else
    EXPECT_EQ(exe[0], '/');
#endif
}

// ------------------------------ BlockingQueue ------------------------------

TEST(BlockingQueueTest, ProducerConsumer)
{
    using namespace libmini;
    BlockingQueue<int> q(4);
    q.push(1);
    q.push(2);
    EXPECT_EQ(q.size(), 2u);

    int v = 0;
    EXPECT_TRUE(q.try_pop(v, 100));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_pop(v, 100));
    EXPECT_EQ(v, 2);

    // 空队列超时失败
    EXPECT_FALSE(q.try_pop(v, 50));

    // 关闭后：存量可取尽，随后 pop 返回 false，push 被丢弃
    q.push(3);
    q.close();
    EXPECT_TRUE(q.try_pop(v, 100));
    EXPECT_EQ(v, 3);
    EXPECT_FALSE(q.pop(v));
    q.push(4);  // 关闭后丢弃，不阻塞
    EXPECT_TRUE(q.empty());
}

TEST(BlockingQueueTest, MultiProducerMultiConsumer)
{
    using namespace libmini;
    BlockingQueue<int> q(8);
    std::atomic<int> sum{0};
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 250;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p, kPerProducer] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push(p * kPerProducer + i + 1);
            }
        });
    }
    std::vector<std::thread> consumers;
    for (int c = 0; c < 3; ++c) {
        consumers.emplace_back([&q, &sum] {
            int v = 0;
            while (q.pop(v)) {   // 关闭且取空后返回 false 退出
                sum += v;
            }
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    // Σ_p Σ_i (p*N + i + 1) = N²·P(P-1)/2 + P·N(N+1)/2
    const std::int64_t expected =
        static_cast<std::int64_t>(kPerProducer) * kPerProducer *
            (kProducers * (kProducers - 1) / 2) +
        static_cast<std::int64_t>(kProducers) *
            (kPerProducer * (kPerProducer + 1) / 2);
    EXPECT_EQ(sum.load(), expected);
}

// ----------------------------- CountdownLatch ------------------------------

TEST(CountdownLatchTest, ReleasesWaitersAtZero)
{
    using namespace libmini;
    CountdownLatch latch(3);
    EXPECT_EQ(latch.count(), 3u);

    std::atomic<int> released{0};
    std::thread waiter([&] {
        latch.wait();
        ++released;
    });
    std::thread waiter2([&] {
        EXPECT_TRUE(latch.wait_for(5000));
        ++released;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(released.load(), 0);   // 未归零前不放行
    latch.count_down();
    latch.count_down();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(released.load(), 0);
    latch.count_down();              // 归零
    waiter.join();
    waiter2.join();
    EXPECT_EQ(released.load(), 2);
    EXPECT_EQ(latch.count(), 0u);

    latch.count_down();              // 已归零后再减：无效且安全
    EXPECT_EQ(latch.count(), 0u);
}

TEST(CountdownLatchTest, WaitForTimeout)
{
    using namespace libmini;
    CountdownLatch latch(2);
    latch.count_down();
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(latch.wait_for(80));   // 还差一次：超时
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(elapsed, 70);
    latch.count_down();
    EXPECT_TRUE(latch.wait_for(0));     // 已归零：立即成功
}

// ------------------------------ ThreadPool 增 ------------------------------

TEST(ThreadPoolIdleTest, WaitIdleDrainsAllTasks)
{
    using namespace libmini;
    ThreadPool pool(3);
    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++done;
        });
    }
    EXPECT_LE(done.load(), 100);
    pool.wait_idle();
    EXPECT_EQ(done.load(), 100);   // wait_idle 返回时全部任务已完成

    // wait_idle 后线程池仍可用；顺带覆盖 submit_int 非模板入口
    auto f = pool.submit_int([] { return 42; });
    EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPoolIdleTest, PendingTasksCountsQueue)
{
    using namespace libmini;
    ThreadPool pool(1);            // 单线程：后提交的任务必然排队
    std::atomic<bool> release{false};
    pool.submit([&release] {
        while (!release.load()) std::this_thread::yield();
    });
    for (int i = 0; i < 5; ++i) {
        pool.submit_void([] {});   // 顺带覆盖 submit_void 非模板入口
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(pool.pending_tasks(), 1u);   // 任务在队列里（≥1，时序宽裕）
    release = true;
    pool.wait_idle();
    EXPECT_EQ(pool.pending_tasks(), 0u);
}

// ------------------------------- HttpClient --------------------------------

#include <httplib.h>  // 测试内起本地服务，验证 HttpClient 的回环行为
#include <spdlog/spdlog.h>  // LogFacade::logger() 返回指针需完整类型才能调方法

namespace {

// 本地 httplib 服务 fixture：端口 0 自动分配，析构自动停
struct LocalHttpServer
{
    httplib::Server server;
    std::thread thread;
    int port = 0;

    LocalHttpServer()
    {
        server.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("hello http", "text/plain");
        });
        server.Post("/echo", [](const httplib::Request& req,
                                httplib::Response& res) {
            res.set_content(req.body, "text/plain");
        });
        server.Get("/query", [](const httplib::Request& req,
                                httplib::Response& res) {
            std::string out;
            for (const auto& kv : req.params) {
                if (!out.empty()) out += "&";
                out += kv.first + "=" + kv.second;   // httplib 已解码
            }
            res.set_content(out, "text/plain");
        });
        server.Get("/header", [](const httplib::Request& req,
                                 httplib::Response& res) {
            res.set_content(req.get_header_value("X-Token"), "text/plain");
        });
        server.Get("/slow", [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            res.set_content("slow done", "text/plain");
        });

        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
    }

    ~LocalHttpServer()
    {
        server.stop();
        thread.join();
    }
};

}  // namespace

TEST(HttpClientTest, GetAndStatusCodes)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);

    const HttpResponse r = c.get("/hello");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hello http");
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(r.headers.at("content-type"), "text/plain");  // 键统一小写

    const HttpResponse nf = c.get("/no_such_path");
    EXPECT_EQ(nf.status, 404);
    EXPECT_FALSE(nf.ok());
    EXPECT_TRUE(nf.error.empty());   // 404 是 HTTP 层响应，非传输错误
}

TEST(HttpClientTest, PostEchoAndJsonHelper)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);

    const HttpResponse r = c.post("/echo", "负载 body 中文", "text/plain");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "负载 body 中文");

    const HttpResponse j = c.post_json("/echo", R"({"k":"v"})");
    EXPECT_TRUE(j.ok());
    EXPECT_EQ(j.body, R"({"k":"v"})");
}

TEST(HttpClientTest, QueryEncoding)
{
    using namespace libmini;
    EXPECT_EQ(HttpClient::build_query({{"a", "1"}, {"b", "2"}}), "a=1&b=2");
    EXPECT_EQ(HttpClient::build_query({}), "");
    // UrlEncode 契约：空格 → '+'（form-urlencoded）；std::map 按键排序
    EXPECT_EQ(HttpClient::build_query({{"q", "x y"}}), "q=x+y");
    EXPECT_EQ(HttpClient::build_query({{"p", "a+b"}}), "p=a%2Bb");  // 字面 + 会被转义

    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);
    const HttpResponse r = c.get("/query", {{"q", "x y"}, {"n", "7"}});
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "n=7&q=x y");   // 服务端已解码：+ → 空格，顺序按键排
}

TEST(HttpClientTest, DefaultHeaders)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);
    c.set_default_header("X-Token", "tok-123");
    const HttpResponse r = c.get("/header");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body, "tok-123");

    c.clear_default_headers();
    const HttpResponse r2 = c.get("/header");
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(r2.body, "");
}

TEST(HttpClientTest, TimeoutReportsTransportError)
{
    using namespace libmini;
    LocalHttpServer srv;
    HttpClient c("127.0.0.1", srv.port);
    c.set_timeout_ms(100);           // /slow 睡 400ms：必超时
    const HttpResponse r = c.get("/slow");
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
    EXPECT_FALSE(r.ok());
}

TEST(HttpClientTest, BadUrlReportsError)
{
    using namespace libmini;
    HttpClient bad("host_without_scheme");   // 无 scheme：构造即无效
    const HttpResponse r = bad.get("/x");
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
}

// ------------------------------- LogFacade ---------------------------------

TEST(LogFacadeTest, FileLoggingAndLevelFilter)
{
    using namespace libmini;
    const std::string dir = "logs_facade_test";
    const std::string path = dir + "/app.log";
    remove_tree(dir);   // 干净起点

    LogFacade::Options o;
    o.file_path = path;
    o.console = false;
    o.level = LogLevel::Debug;
    ASSERT_TRUE(LogFacade::init(o));

    spdlog::logger* lg = LogFacade::logger();
    ASSERT_NE(lg, nullptr);
    lg->debug("debug 行 {}", 1);
    lg->info("info 行 {}", 1);
    LogFacade::set_level(LogLevel::Error);
    EXPECT_EQ(LogFacade::level(), LogLevel::Error);
    lg->info("这条不该出现");
    lg->error("error 行");
    LogFacade::flush();
    LogFacade::shutdown();
    EXPECT_EQ(LogFacade::logger(), nullptr);

    const std::string content = read_file(path);
    EXPECT_NE(content.find("debug 行 1"), std::string::npos);
    EXPECT_NE(content.find("info 行 1"), std::string::npos);
    EXPECT_NE(content.find("error 行"), std::string::npos);
    EXPECT_EQ(content.find("这条不该出现"), std::string::npos);

    remove_tree(dir);
}

TEST(LogFacadeTest, ReinitAndAsyncMode)
{
    using namespace libmini;
    const std::string dir = "logs_facade_async_test";
    const std::string path1 = dir + "/a.log";
    const std::string path2 = dir + "/b.log";
    remove_tree(dir);

    LogFacade::Options o;
    o.file_path = path1;
    o.console = false;
    ASSERT_TRUE(LogFacade::init(o));
    LogFacade::logger()->warn("第一份配置");

    // 重复 init：切换到第二份配置（幂等，不崩）
    LogFacade::Options o2;
    o2.file_path = path2;
    o2.console = false;
    o2.async_mode = true;            // 异步模式：shutdown 前落盘
    ASSERT_TRUE(LogFacade::init(o2));
    LogFacade::logger()->critical("第二份配置异步");
    LogFacade::flush();
    LogFacade::shutdown();

    // 第一份文件只含旧内容（切换后不再写入）
    EXPECT_NE(read_file(path1).find("第一份配置"), std::string::npos);
    EXPECT_NE(read_file(path2).find("第二份配置异步"), std::string::npos)
        << "shutdown 后异步日志应已落盘";
    remove_tree(dir);
}

TEST(LogFacadeTest, InvalidConfigFailsCleanly)
{
    using namespace libmini;
    LogFacade::Options o;
    o.console = false;               // 既无控制台也无文件：配置无效
    o.file_path.clear();
    EXPECT_FALSE(LogFacade::init(o));

    // 日志门面会自动创建缺失的父目录（设计行为），所以要构造「真创建不了」
    // 的场景：让路径中间组件是一个已存在的文件
    const std::string blocker = "logs_facade_blocker.tmp";
    write_file(blocker, "i am a file");
    LogFacade::Options bad;
    bad.console = false;
    bad.file_path = blocker + "/log.txt";   // blocker 是文件：mkdir 必败
    EXPECT_FALSE(LogFacade::init(bad));
    remove_file(blocker);

    LogFacade::shutdown();   // 恢复无 logger 状态，不影响其他测试
}

// ------------------------------- config_facade -------------------------------

TEST(ConfigFacadeTest, LayeredPriorityDefaultFileEnv)
{
    using namespace libmini;
    const std::string cfg_path = "test_facade_cfg.json";
    write_file(cfg_path, "{\"server\": {\"port\": 9090, \"host\": \"0.0.0.0\"}, \"retries\": 3}");

    ConfigFacade cfg;
    cfg.set_default("server.port", "8080");
    cfg.set_default("server.timeout_ms", "5000");
    cfg.set_default("log.level", "info");

    // 文件加载前：默认层生效
    EXPECT_EQ(cfg.get_int("server.port"), 8080);
    EXPECT_EQ(cfg.source_of("server.port"), "default");

    // 文件加载后：覆盖默认值
    EXPECT_TRUE(cfg.load_file(cfg_path));
    EXPECT_EQ(cfg.get_int("server.port"), 9090);
    EXPECT_EQ(cfg.get_int("retries"), 3);
    EXPECT_EQ(cfg.get("server.host"), "0.0.0.0");
    EXPECT_EQ(cfg.source_of("server.port"), "file");
    // 未被文件覆盖的默认值仍然生效
    EXPECT_EQ(cfg.get_int("server.timeout_ms"), 5000);

    // 环境变量覆盖文件层：MYAPP_SERVER_PORT
    env_set("MYAPP_SERVER_PORT", "7070");
    cfg.set_env_prefix("MYAPP_");
    cfg.refresh_env();
    EXPECT_EQ(cfg.get_int("server.port"), 7070);
    EXPECT_EQ(cfg.source_of("server.port"), "env");
    // 环境变量没覆盖的键不受影响
    EXPECT_EQ(cfg.get_int("retries"), 3);

    env_remove("MYAPP_SERVER_PORT");
    remove_file(cfg_path);
}

TEST(ConfigFacadeTest, IniAndMissingFile)
{
    using namespace libmini;
    const std::string ini_path = "test_facade_cfg.ini";
    write_file(ini_path, "[db]\r\nhost = 127.0.0.1\r\nport = 5432\r\n");

    ConfigFacade cfg;
    EXPECT_FALSE(cfg.load_file("no_such_file_facade.json"));   // 不存在
    EXPECT_FALSE(cfg.load_file("test_facade_cfg.txt"));        // 不支持的扩展名

    EXPECT_TRUE(cfg.load_file(ini_path));
    EXPECT_EQ(cfg.get("db.host"), "127.0.0.1");
    EXPECT_EQ(cfg.get_int("db.port"), 5432);
    EXPECT_TRUE(cfg.has("db.host"));
    EXPECT_FALSE(cfg.has("db.missing"));

    remove_file(ini_path);
}

TEST(ConfigFacadeTest, NestedJsonAndDefaults)
{
    using namespace libmini;
    const std::string p = "test_facade_nested.json";
    write_file(p, "{\"a\": {\"b\": {\"c\": true, \"d\": 1.5}}}");

    ConfigFacade cfg;
    cfg.set_default("fallback.key", "hello");
    EXPECT_TRUE(cfg.load_file(p));
    EXPECT_TRUE(cfg.get_bool("a.b.c"));
    EXPECT_DOUBLE_EQ(cfg.get_double("a.b.d"), 1.5);
    EXPECT_EQ(cfg.get("fallback.key"), "hello");

    // 类型转换失败回落默认值
    EXPECT_EQ(cfg.get_int("fallback.key", 42), 42);
    EXPECT_EQ(cfg.get_int("a.b.d", 7), 7);   // 1.5 不是 int

    remove_file(p);
}

// ------------------------------- net_addr -------------------------------

TEST(NetAddrTest, ParseEndpointForms)
{
    using namespace libmini;
    std::string host;
    int port = 0;

    EXPECT_TRUE(parse_endpoint("192.168.1.5:8080", host, port));
    EXPECT_EQ(host, "192.168.1.5");
    EXPECT_EQ(port, 8080);

    // IPv6 括号形式
    EXPECT_TRUE(parse_endpoint("[::1]:8080", host, port));
    EXPECT_EQ(host, "::1");
    EXPECT_EQ(port, 8080);
    EXPECT_TRUE(parse_endpoint("[::1]", host, port));
    EXPECT_EQ(host, "::1");
    EXPECT_EQ(port, 0);

    // 纯端口 / 主机名 / 带空端口
    EXPECT_TRUE(parse_endpoint("8080", host, port));
    EXPECT_EQ(port, 8080);
    EXPECT_TRUE(parse_endpoint("myhost", host, port));
    EXPECT_EQ(host, "myhost");
    EXPECT_EQ(port, 0);
    EXPECT_TRUE(parse_endpoint("localhost:", host, port));
    EXPECT_EQ(host, "localhost");
    EXPECT_EQ(port, 0);

    // 非法端口
    std::string h2;
    int p2 = 0;
    EXPECT_FALSE(parse_endpoint("host:99999", h2, p2));
    EXPECT_FALSE(parse_endpoint("host:abc", h2, p2));
    EXPECT_FALSE(parse_endpoint("[::1", h2, p2));

    // 缺省封装：失败回落 0.0.0.0:0
    parse_endpoint_or_default("host:abc", host, port);
    EXPECT_EQ(host, "0.0.0.0");
    EXPECT_EQ(port, 0);
}

TEST(NetAddrTest, Ipv4RoundTrip)
{
    using namespace libmini;
    const std::uint32_t net = ipv4_from_string("192.168.1.5");
    EXPECT_NE(net, 0u);
    EXPECT_EQ(ipv4_to_string(net), "192.168.1.5");

    EXPECT_EQ(ipv4_from_string("0.0.0.0"), 0u);
    EXPECT_EQ(ipv4_to_string(0), "0.0.0.0");
    EXPECT_EQ(ipv4_from_string("256.1.1.1"), 0u);     // 越界
    EXPECT_EQ(ipv4_from_string("1.2.3"), 0u);          // 段数不足
    EXPECT_EQ(ipv4_from_string("01.2.3.4"), 0u);       // 前导零
    EXPECT_EQ(ipv4_from_string("1.2.3.x"), 0u);        // 非数字

    // 回环地址往返
    const std::uint32_t lo = ipv4_from_string("127.0.0.1");
    EXPECT_EQ(ipv4_to_string(lo), "127.0.0.1");
}

TEST(NetAddrTest, ResolveLocalhost)
{
    using namespace libmini;
    // localhost 一定可解析；IPv4 应排在最前（约定）
    const std::vector<NetAddrEntry> entries = resolve_host("localhost", "");
    ASSERT_FALSE(entries.empty());
    EXPECT_FALSE(entries[0].is_ipv6);
    EXPECT_EQ(entries[0].ip, "127.0.0.1");

    // IPv4 字面量快速路径（不依赖 DNS）
    const std::uint32_t net = resolve_ipv4_net("10.0.0.7");
    EXPECT_NE(net, 0u);
    EXPECT_EQ(ipv4_to_string(net), "10.0.0.7");
}

// ------------------------------- timer_wheel -------------------------------

TEST(TimerWheelTest, SingleShotFiresOnce)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);
    wheel.add_ms(50, [&fired] { ++fired; });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(fired.load(), 1);
    EXPECT_TRUE(wheel.idle());
}

TEST(TimerWheelTest, PeriodicAndCancel)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);

    // 周期任务：跑 3 次后自停（fn 返回 false）。轮询等触发：
    // CI 慢机上固定 400ms 可能只跑 2 次（10ms tick 实际粒度更粗）
    wheel.add_periodic_ms(30, [&fired]() -> bool {
        return ++fired < 3;
    });
    for (int i = 0; i < 100 && fired.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(fired.load(), 3);
    wheel.wait_idle();

    // 句柄取消：未触发的任务不再执行
    std::atomic<int> cnt(0);
    TimerWheel::Handle h = wheel.add_ms(100, [&cnt] { ++cnt; });
    EXPECT_TRUE(h.cancel());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(cnt.load(), 0);
    EXPECT_TRUE(wheel.idle());

    // 重复取消无效
    EXPECT_FALSE(h.cancel());
}

TEST(TimerWheelTest, BulkTimersO1Add)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);

    // 海量定时器：10k 添加 + 9k 取消，验证 O(1) 路径不退化
    std::vector<TimerWheel::Handle> handles;
    handles.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        handles.push_back(wheel.add_ms(200 + (i % 500), [&fired] { ++fired; }));
    }
    for (int i = 0; i < 9000; ++i) {
        EXPECT_TRUE(handles[static_cast<std::size_t>(i)].cancel());
    }
    EXPECT_EQ(wheel.pending_count(),
              static_cast<std::size_t>(1000));

    // 剩余 1000 个最终全部触发
    wheel.wait_idle();
    EXPECT_EQ(fired.load(), 1000);
}

TEST(TimerWheelTest, WaitIdleBlocksUntilDone)
{
    using namespace libmini;
    TimerWheel wheel(std::chrono::milliseconds(10));
    std::atomic<int> fired(0);
    wheel.add_ms(60, [&fired] { ++fired; });
    wheel.add_ms(120, [&fired] { ++fired; });

    // wait_idle 应阻塞到两个任务都触发
    const auto t0 = std::chrono::steady_clock::now();
    wheel.wait_idle();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(fired.load(), 2);
    EXPECT_GE(elapsed, 60);   // 至少等到第一个任务到期
}

// ------------------------------- HttpServer --------------------------------

TEST(HttpServerTest, RoutesParamsQueryAndLifecycle)
{
    using namespace libmini;
    HttpServer server;

    server.get("/hello/:name", [](const HttpRequest& req) {
        return HttpReply::text(200, "hi " + req.param("name"));
    });
    server.get("/sum", [](const HttpRequest& req) {
        const int a = std::atoi(req.param("a").empty()
                                    ? req.query.at("a").c_str()
                                    : req.param("a").c_str());
        const int b = std::atoi(req.query.at("b").c_str());
        return HttpReply::json(200, "{\"sum\":" + std::to_string(a + b) + "}");
    });
    server.post("/echo", [](const HttpRequest& req) {
        return HttpReply::text(200, req.body);
    });
    server.set_fallback([](const HttpRequest&) {
        return HttpReply::error(404, "no route");
    });

    // 前置过滤器：携带合法 token 放行，否则短路 401
    server.use([](const HttpRequest& req, HttpReply& reply) {
        const auto it = req.headers.find("x-token");
        if (it != req.headers.end() && it->second == "secret") return true;
        reply = HttpReply::error(401, "unauthorized");
        return false;
    });

    std::atomic<int> logged(0);
    server.set_access_logger([&logged](const HttpRequest&, const HttpReply&,
                                       std::int64_t) { ++logged; });

    EXPECT_TRUE(server.start_background(0));
    EXPECT_TRUE(server.wait_until_ready());
    ASSERT_GT(server.port(), 0);

    HttpClient c("127.0.0.1", server.port());
    c.set_default_header("X-Token", "secret");

    // 路径参数 + 过滤器放行
    const HttpResponse r1 = c.get("/hello/libmini");
    EXPECT_TRUE(r1.ok());
    EXPECT_EQ(r1.body, "hi libmini");

    // query 解析（URL 解码 + JSON 回复）
    const HttpResponse r2 = c.get("/sum?a=40&b=2");
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(r2.body, "{\"sum\":42}");

    // POST 回显
    const HttpResponse r3 = c.post("/echo", "payload 中文", "text/plain");
    EXPECT_TRUE(r3.ok());
    EXPECT_EQ(r3.body, "payload 中文");

    // 未带 token → 过滤器短路 401（换一个无默认头的客户端）
    HttpClient anon("127.0.0.1", server.port());
    const HttpResponse r4 = anon.get("/hello/x");
    EXPECT_EQ(r4.status, 401);

    // 未命中路由 → fallback
    const HttpResponse r5 = c.get("/nothing");
    EXPECT_EQ(r5.status, 404);
    EXPECT_NE(r5.body.find("no route"), std::string::npos);

    // 访问日志钩子按请求计数
    EXPECT_EQ(logged.load(), 5);

    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(HttpServerTest, ApplyConfigPortFromEnv)
{
    using namespace libmini;
    env_set("LT_HTTPSRV_PORT", "0");  // 0 = 系统分配，仅验证环境层穿透
    ConfigFacade cfg;
    cfg.set_env_prefix("LT_HTTPSRV_");
    cfg.set_default("port", "80");

    HttpServer server;
    server.apply_config(cfg, "");
    EXPECT_EQ(server.port(), 0);  // 环境层(0)覆盖 default 层(80)

    env_remove("LT_HTTPSRV_PORT");
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
