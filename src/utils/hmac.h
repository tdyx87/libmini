#ifndef LIBMINI_HMAC_H
#define LIBMINI_HMAC_H

#include <string>

#include "libmini.h"
#include "digest.h"

namespace libmini {

// HMAC（RFC 2104）：Hash-based Message Authentication Code。
// 典型用途：云服务 API 签名（AWS SigV4、阿里云等都是 HMAC-SHA256）、
// Webhook 验签、token 派生。
//
// 用法与 digest 一致——一次性接口为主，增量接口供大数据流式场景：
//   HmacSha256::hex(key, data)                     → 64 字符小写十六进制
//   HmacSha256 h; h.set_key(key); h.update(a); ... → 流式
//
// 注意：set_key 在 update 之前调用；重复调用会重新初始化内部状态。

class LIBMINI_API HmacSha256 {
public:
    HmacSha256() { reset(); }

    // 重新开始一次计算；key 可为空（此时 ipad/opad 为固定填充）
    void reset();
    void set_key(const std::string& key);

    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出 32 字节 MAC（之后可 set_key/reset 复用）
    std::string finish();

    // 一次性计算，返回 32 字符小写十六进制
    static std::string hex(const std::string& key, const std::string& data);

    // 一次性计算，返回 32 字节原始 MAC
    static std::string raw(const std::string& key, const std::string& data);

private:
    Sha256 inner_;             // 内层（key ^ ipad 后的消息流）
    Sha256 outer_;             // 外层（key ^ opad 后接内层摘要）
    bool keyed_ = false;
};

// HMAC-MD5：老协议兼容用（如部分短信网关、旧系统），新代码请用 HMAC-SHA256
class LIBMINI_API HmacMd5 {
public:
    HmacMd5() { reset(); }

    void reset();
    void set_key(const std::string& key);

    void update(const void* data, std::size_t size);
    void update(const std::string& data) { update(data.data(), data.size()); }

    // 结束并输出 16 字节 MAC（之后可 set_key/reset 复用）
    std::string finish();

    // 一次性计算，返回 32 字符小写十六进制
    static std::string hex(const std::string& key, const std::string& data);

    // 一次性计算，返回 16 字节原始 MAC
    static std::string raw(const std::string& key, const std::string& data);

private:
    Md5 inner_;
    Md5 outer_;
    bool keyed_ = false;
};

}  // namespace libmini

#endif  // LIBMINI_HMAC_H
