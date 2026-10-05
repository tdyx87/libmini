#ifndef LIBMINI_UUID_H
#define LIBMINI_UUID_H

#include <cstdint>
#include <string>

#include "libmini.h"

namespace libmini {

// UUID：v4 随机、v7 时间有序、v5 命名空间派生，对应 boost::uuids::uuid。
// 版本号沿用 RFC 4122 的编号（v5/v7），格式细节按 RFC 9562 的后续修订。
struct LIBMINI_API Uuid {
    std::uint8_t bytes[16];

    // 生成一个随机 UUID（v4，变体位按 RFC 4122 设置）
    static Uuid generate();

    // 生成时间有序 UUID（RFC 9562 §5.7，版本号沿用 v7）。
    //
    // 为什么需要它：v4 的 122 位随机性意味着插入顺序与 ID 顺序无关。
    // 用 v4 当数据库主键时，B+ 树每次插入都落在随机页面上，页分裂、
    // 页缓存命中率、写入放大全都随数据量劣化——同一批数据 v4 主键的
    // 索引体积常比顺序主键大一到数倍。v7 前 48 位是 Unix 毫秒时间戳，
    // 排序即时间序，写入密集场景是数量级的差别。
    //
    // 同一毫秒内用进程内计数器递增填充高位随机位，保证同一进程产出的
    // v7 **严格单调递增**：否则时间戳相同的多个 ID 之间顺序仍是随机的，
    // 前缀聚簇的收益会打折。计数器只保证单进程，多副本部署时不同进程
    // 的同毫秒 ID 之间仍是随机序（规范对此明确不要求全局有序）。
    static Uuid generate_v7();

    // 同上，但时间戳由调用方指定（Unix 毫秒）。用于回填历史数据、
    // 导入外部系统、可复现的测试。不做单调性处理——时间戳可以倒退。
    static Uuid generate_v7(std::int64_t unix_ms);

    // v5 命名空间派生（RFC 4122 §4.3，内部用 SHA-1）。
    // 同一 namespace + 同一 name 永远得到同一个 UUID，不依赖状态。
    static Uuid v5(const Uuid& name_space, const std::string& name);

    // 预定义命名空间（RFC 4122 §4.1.3）。自建命名空间直接构造 Uuid 并填
    // bytes[16] 即可——它不必是合法 UUID，命名空间在算法里只是个前缀。
    static Uuid namespace_dns();
    static Uuid namespace_url();
    static Uuid namespace_oid();
    static Uuid namespace_x500();

    // 版本号（高 4 位）：1=v1 3=v3 4=v4 5=v5 7=v7，其他值原样返回
    int version() const;

    // 变体位（byte[8] 高 2 位）：0=NCS 2=RFC4122 6=Microsoft 7=未来保留
    int variant() const;

    // v7 的 Unix 毫秒时间戳；非 v7 返回 0（无法从 v4 里推出时间）
    std::int64_t timestamp_ms() const;

    // 标准格式解析："xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（大小写均可，
    // 也接受不带连字符的 32 位十六进制形式）。失败返回 false。
    static bool parse(const std::string& str, Uuid& out);

    // 标准格式输出（小写、带连字符）
    std::string to_string() const;

    // 不带连字符的 32 位十六进制形式（小写）
    std::string to_hex_string() const;

    bool operator==(const Uuid& other) const;
    bool operator!=(const Uuid& other) const;
    bool operator<(const Uuid& other) const;  // 用于 std::map 键

    // 是否为全零 UUID
    bool is_nil() const;

    // 全零 UUID
    static Uuid nil();
};

}  // namespace libmini

#endif  // LIBMINI_UUID_H
