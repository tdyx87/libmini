#ifndef LIBMINI_HARDWARE_INFO_H
#define LIBMINI_HARDWARE_INFO_H

#include <cstdint>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// 硬件清单查询（一次性快照，非运行时指标）。
//
// 与 utils/system_info.h 的分工：
//   system_info —— 运行时指标（CPU 核数、内存余量、磁盘剩余空间、PID…），
//                  适合诊断日志里高频打印的字段；
//   hardware_info —— 硬件「是什么」的静态清单（CPU 型号、网卡与 MAC/IP、
//                  磁盘与卷标识、主板/BIOS），适合做机器指纹、许可证绑定、
//                  资产上报、崩溃报告头部的「环境概览」。
//
// 通用约定（跨平台一致）：
//   - 全部是快照式查询，不缓存、不后台刷新；
//   - 任何平台差异都通过「字段留空 + error 字段说明原因」表达，绝不抛异常；
//   - 取不到序列号/标识的常见原因是权限不足（物理盘、DMI product_serial
//     多为 root/管理员）、驱动不支持、或运行在容器/CI 里。库不做提权，
//     也不会为了取字段 fork 提权进程；
//   - 空列表是合法结果（例如 macOS 上 system_profiler 被禁用时磁盘列表为空）。
//
// 已知限制（Windows 磁盘型号）：STORAGE_DEVICE_DESCRIPTOR 里的型号/序列号是
// 驱动填的定长 ANSI 字段，编码因驱动而异。库按「UTF-8 严格校验 → 系统 ANSI
// 代码页」两档处理，能正确读出 ASCII 与中文（GBK）型号；但极少数虚拟盘驱动
// 会把 UTF-16 写进 ANSI 字段，这类数据会在首个 NUL 字节处被截断，无法可靠
// 还原——遇到时对应字段为空且 serial_error 说明原因，不影响其余字段。

// ---------------- CPU ----------------

struct CpuInfo
{
    std::string vendor;         // GenuineIntel / AuthenticAMD / Apple …
    std::string brand;          // "12th Gen Intel(R) Core(TM) i7-12700K"
    std::string architecture;   // x86_64 / aarch64 / arm64
    int logical_cores = 0;      // 逻辑核（含超线程）
    int physical_cores = 0;     // 物理核；0 = 取不到
    int max_frequency_mhz = 0;  // 标称频率；0 = 取不到
    bool hyperthreading = false;// physical > 0 且 logical > physical 时为 true
    std::string error;          // 品牌/厂商取不到时的原因；整体取不到才非空

    bool empty() const
    {
        return brand.empty() && vendor.empty() && logical_cores == 0;
    }
};

// ---------------- 网络适配器 ----------------

struct NetworkAdapterInfo
{
    std::string name;         // 友好名（Windows）/ 接口名（Linux、macOS）
    std::string description;  // 适配器描述；可能为空
    std::string mac_address;  // 规范化小写冒号形式 "aa:bb:cc:dd:ee:ff"；无则空
    std::string mac_error;    // 无 MAC 的原因（无权限/接口无 MAC/虚拟网卡等）
    std::vector<std::string> ipv4_addresses;
    std::vector<std::string> ipv6_addresses;
    bool is_loopback = false;
    bool is_up = false;       // 链路是否 UP

    bool has_mac() const { return !mac_address.empty(); }
};

// ---------------- 物理磁盘 ----------------

struct DiskInfo
{
    std::string device_path;    // \\.\PhysicalDrive0 / /dev/nvme0n1
    std::string model;          // 型号；可能为空
    std::string serial_number;  // 序列号；取不到时为空，看 serial_error
    std::string serial_error;   // 取不到序列号的原因（权限/驱动/虚拟盘）
    std::string interface_type; // SATA / NVMe / USB / SD / Virtual / Unknown
    std::uint64_t size_bytes = 0;
    bool is_removable = false;
};

// ---------------- 卷（挂载点 / 文件系统） ----------------

struct VolumeInfo
{
    std::string mount_point;    // "C:\" 或 "/"
    std::string label;          // 卷标；可能为空
    std::string filesystem;     // NTFS / ext4 / apfs …
    std::string serial_number;  // 卷标识；取不到时为空，看 serial_error
    std::string serial_error;
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
};

// ---------------- 主板 / BIOS ----------------

struct BiosInfo
{
    std::string system_vendor;      // 整机厂商
    std::string system_model;       // 整机型号
    std::string bios_vendor;
    std::string bios_version;
    std::string bios_release_date;  // 形如 "07/15/2024"
    std::string serial_number;      // 主板/整机序列号；取不到时为空
    std::string serial_error;       // 常见原因：需要管理员/root

    bool empty() const
    {
        return system_vendor.empty() && system_model.empty() &&
               bios_vendor.empty() && serial_number.empty();
    }
};

// ---------------- 查询接口 ----------------

LIBMINI_API CpuInfo cpu_info();

// 全部网络适配器（含回环；按系统枚举顺序返回）
LIBMINI_API std::vector<NetworkAdapterInfo> network_adapters();

// 全部物理磁盘（虚拟盘会标注 interface_type = "Virtual"）
LIBMINI_API std::vector<DiskInfo> disks();

// 已挂载的真实卷（过滤掉 proc/sys/devpts 等伪文件系统）
LIBMINI_API std::vector<VolumeInfo> volumes();

LIBMINI_API BiosInfo bios_info();

// 便捷取用：首个「非回环且 UP」的适配器 MAC；没有则返回空串
LIBMINI_API std::string primary_mac_address();

// 便捷取用：首个「非回环且 UP」的适配器的 IPv4；没有则返回空串
LIBMINI_API std::string primary_ipv4_address();

// 把任意分隔的十六进制 MAC（"AA-BB-CC-DD-EE-FF" / "aabbccddeeff"）规范化为
// 小写冒号形式；输入为空或长度不合法时原样返回
LIBMINI_API std::string format_mac(const std::string& raw);

}  // namespace libmini

#endif  // LIBMINI_HARDWARE_INFO_H