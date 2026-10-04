#ifndef LIBMINI_MACHINE_FINGERPRINT_H
#define LIBMINI_MACHINE_FINGERPRINT_H

#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 机器指纹：在 hardware_info 的硬件清单之上，派生出「这台机器是谁」的稳定标识。
//
// 用途：许可证单机绑定、席位去重上报、遥测/崩溃报告的设备聚合。
//
// 与 hardware_info 的分工：
//   hardware_info      —— 原始硬件清单（人可读的全部字段，诊断用）；
//   machine_fingerprint —— 从清单里挑出「稳定且能区分个体」的少数信号，
//                          归一化后哈希成固定长度标识（机器可读）。
//
// 三条硬约束（决定了下面所有取舍）：
//
//  1. **只认几乎不变的信号当主导信号**。主板/整机序列号最稳；其次 CPU 型号；
//     网卡 MAC 与物理盘序列号属于「会变」的一档（换网卡、换盘、系统开随机
//     物理地址都会变），因此由 FingerprintPolicy 单独控制是否参与。
//
//  2. **占位符序列号必须过滤**。DMI/WMI 的 product_serial 在大量 OEM 机器上
//     是 "To Be Filled By O.E.M."、"Default string"、"System Serial Number"
//     这类没填的模板值（macOS 会给 "null"）。不过滤的话，同一整机厂所有
//     没填序列号的机器会算出**同一个指纹**，许可证绑定直接失效。
//     canonical_fingerprint_token() 统一处理这一层。
//
//  3. **虚拟网卡 / 虚拟盘不参与**。VMware/VirtualBox/Hyper-V/WSL/Docker/veth/
//     VPN 的 MAC 与磁盘标识在虚机/容器里每次重建都变，混进指纹等于自毁稳定性。
//
// 其他约定：
//   - 纯查询：无缓存、无后台线程、无文件写入、无网络访问、不提权；
//   - 绝不抛异常，取不到的信号通过 missing 字段 + confidence 表达；
//   - 一个信号都取不到时 id 为空串、confidence 为 0 —— 这是合法结果（受限
//     容器里可能一个都没有）。调用方必须检查 empty()，不能假定有值。
//
// 关于「刻意不做」的事：没有采用 Linux 的 /etc/machine-id。它按「安装实例」
// 而非「机器」标识，容器里每个实例各不相同（云主机的容器镜像模板甚至可能
// 全网同值），会与本模块「标识物理机器」的语义冲突。
//
// 版本与迁移：
//   id 的计算包含 machine_fingerprint_version() 版本串。若将来升级信号集合，
//   id 会随之改变 —— 这是刻意的：静默改变 id 比显式破坏更危险。
//   需要跨 libmini 版本稳定的调用方，应当**同时持久化 signals**，升级后用
//   signals 自行派生；signals 就是为此暴露的稳定中间层。

// 指纹策略：决定「会变」的信号是否参与
enum class FingerprintPolicy
{
    kStable = 0,    // 只用稳定信号：换硬盘/换网卡后指纹不变。熵最低，
                    // 许可证绑定首选（硬件升级不踢用户下线）
    kBalanced = 1,  // kStable + 首个物理网卡 MAC。默认档，兼顾唯一性与耐受度
    kStrict = 2     // kBalanced + 全部物理盘序列号。熵最高，换硬件即失效
};

struct MachineFingerprint
{
    std::string id;           // 64 字符小写十六进制；空串 = 一个信号都没取到
    std::string short_id;     // id 前 16 字符，便于日志 / UI / 命令行显示
    std::string source;       // 主导信号名：board / cpu / mac / disk
    int confidence = 0;       // 0..100，语义见下
    FingerprintPolicy policy = FingerprintPolicy::kBalanced;
    bool is_virtual = false;  // 检出虚拟化环境；此时 confidence 被强制压到 <= 20

    // 实际参与哈希的规范化信号，顺序固定，形如 "board=..." / "cpu=..." /
    // "mac=..." / "disk=..."。同一台机器多次查询结果一致。
    std::vector<std::string> signals;

    // 想用但取不到的信号 + 原因（原因直接透传 hardware_info 的 error 字段，
    // 典型是「需要管理员权限」）。用于回答「为什么这台机器的指纹这么弱」。
    std::vector<std::string> missing;

    bool empty() const { return id.empty(); }
};

// confidence 分档：
//   90..100  主板序列号 + CPU 型号（再叠加 MAC / 盘序列号时更靠上限）
//   45..89   退到 CPU 型号（或盘 / MAC 兜底）
//   21..44   仅 MAC 或仅盘序列号
//   1..20    仅 CPU 型号，且检出虚拟化环境（虚机指纹不可用于授权）
//   0        一个可用信号都没有

// 参与 id 计算的信号集合版本串（当前 "v1"）。改动信号集合时必须升版本，
// 否则同一台机器会在升级前后算出不同的 id。
LIBMINI_API const char* machine_fingerprint_version();

// 便捷入口：kBalanced 策略、不加 salt
LIBMINI_API MachineFingerprint machine_fingerprint();

// 全控制入口。salt 可为空串；非空时混入哈希，用于租户隔离、多产品线隔离，
// 或防止有人拿公开的 SHA-256 彩虹表反查序列号。
LIBMINI_API MachineFingerprint machine_fingerprint_with(
    FingerprintPolicy policy, const std::string& salt);

// 归一化一个原始字段：去首尾空白 + 内部连续空白压成单空格 + 转大写。
// 命中「未填占位符」（见上约束 2）或有效字符少于 4 个时返回**空串**，
// 调用方据此判定该信号不可用。纯函数，可直接单测。
LIBMINI_API std::string canonical_fingerprint_token(const std::string& raw);

// 从任意硬件描述文本（整机厂商 / 型号 / BIOS 厂商）判断是否虚拟化环境。
// 纯函数，可直接单测。
LIBMINI_API bool looks_like_virtual_machine(const std::string& description);

// 从网卡名 / 描述判断是否虚拟 / 隧道 / 虚拟化网卡。纯函数，可直接单测。
LIBMINI_API bool looks_like_virtual_adapter(const std::string& name_or_description);

// 本机是否运行在虚拟化环境里（整机厂商 + 型号 + BIOS 厂商 + 网卡名综合判定）。
// 检出为真时不适合做许可证绑定：虚机快照克隆 / 容器重建都会产生「同一指纹
// 的多台机器」或「指纹突变」。
LIBMINI_API bool is_virtual_machine();

}  // namespace libmini

#endif  // LIBMINI_MACHINE_FINGERPRINT_H