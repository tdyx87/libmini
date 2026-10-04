#include "machine_fingerprint.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "digest.h"
#include "hardware_info.h"
#include "string_utils.h"

namespace libmini {

namespace {

// 哈希域分隔串：保证本模块的 id 与库里其它 SHA-256 用法（文件摘要、口令等）
// 不可能撞值
const char kHashDomain[] = "libmini-machine-fingerprint";

// 信号集合版本串。改动 signals 的构成必须升版本，否则同一台机器会在升级
// 前后算出不同的 id（见头文件「版本与迁移」）
const char kFingerprintVersion[] = "v1";

// 「没填」的占位序列号。转小写后做**包含**匹配：OEM 模板串常带前后缀
// （"To Be Filled By O.E.M. "、"Serial Number: N/A"），精确相等匹配会漏。
// 收录的都是极不可能出现在真实序列号里的片段。
const char* const kPlaceholderKeys[] = {
    "to be filled by o.e.m",
    "to be filled by oem",
    "default string",
    "system serial number",
    "system board",
    "base board",
    "serial number",
    "not specified",
    "not applicable",
    "not available",
    "not provided",
    "no serial",
    "unknown",
    "undefined",
    "null",
    "none",
    "n/a",
    "oem string",
    "changeme",
    "placeholder",
    "00000000",
    "123456789",
};

// 整机厂商 / 型号 / BIOS 厂商文本里的虚拟化与云主机特征
const char* const kMachineVendorKeys[] = {
    "vmware",
    "virtualbox",
    "innotek",
    "vbox",
    "qemu",
    "kvm",
    "bochs",
    "xen",
    "parallels",
    "hyper-v",
    "hyperv",
    "virtual machine",
    "virtual pc",
    "oracle vm",
    "amazon ec2",
    "google compute engine",
    "alibaba cloud",
    "digitalocean",
    "openstack",
};

// 虚拟化软件自己装的网卡。判定「本机是不是虚拟机」时**只看这一组**：
// 容器/VPN 类网卡（docker/wsl/tap/vpn…）在大量物理开发机上同样存在，
// 拿它们判定虚拟机会把真实机器误判为虚机、把 confidence 压到 20。
const char* const kHypervisorAdapterKeys[] = {
    "vmware",
    "virtualbox",
    "innotek",
    "vbox",
    "vmnet",
    "vnic",
    "parallels",
    "hyper-v",
    "hyperv",
    "virtual machine",
    "xen",
    "qemu",
    "bochs",
};

// 一切「不该进指纹」的网卡：虚拟化 + 容器 + 隧道 + VPN + 伪接口 + 蓝牙。
// 比 kHypervisorAdapterKeys 宽，只用于**排除 MAC**。
const char* const kVirtualAdapterKeys[] = {
    "vmware",     "virtualbox", "innotek",     "vbox",      "vmnet",
    "vnic",       "parallels",  "hyper-v",     "hyperv",    "xen",
    "qemu",       "bochs",      "virtual",     "docker",    "veth",
    "wsl",        "tunnel",     "tun",         "tap",       "utun",
    "bridge",     "loopback",   "pseudo",      "npcap",     "vpn",
    "openvpn", "wireguard", "tailscale", "zerotier", "bluetooth",
    "remote nd",
};
// 注意：这一组只用于**排除** MAC，故关键词取得偏宽（宁可漏一枚真实网卡，
// 也不要收进一枚会变的虚拟网卡）。刻意不收 "usb lan" / "generic" / "vm"：
// 它们会误伤真实 USB 网卡与厂商通用描述。

template <std::size_t N>
bool contains_keyword(const std::string& lower, const char* const (&keys)[N])
{
    for (std::size_t i = 0; i < N; ++i) {
        if (lower.find(keys[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 去首尾空白 + 内部连续空白（含 \t\r\n）压成单空格 + 转大写
std::string collapse_and_upper(const std::string& raw)
{
    const std::string s = to_upper(trim(raw));
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(s[i]);
    }
    return out;
}

std::size_t alnum_count(const std::string& s)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (std::isalnum(static_cast<unsigned char>(s[i]))) {
            ++n;
        }
    }
    return n;
}

// 忽略分隔符后是否只剩同一种字符："00000000"、"XX-XXXXXX"、"--------"、全 0 MAC
bool is_uniform_token(const std::string& s)
{
    char first = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!std::isalnum(static_cast<unsigned char>(s[i]))) {
            continue;
        }
        if (count == 0) {
            first = s[i];
        } else if (s[i] != first) {
            return false;
        }
        ++count;
    }
    return count >= 4;
}

std::string canonical(const std::string& raw)
{
    const std::string s = collapse_and_upper(raw);
    if (s.empty()) {
        return std::string();
    }
    if (contains_keyword(to_lower(s), kPlaceholderKeys)) {
        return std::string();
    }
    if (alnum_count(s) < 4 || is_uniform_token(s)) {
        return std::string();
    }
    return s;
}

// 候选物理网卡 MAC。**不要求链路 UP**：笔记本合盖 / 网线拔掉会让指纹突变，
// 这是比「枚举到哪枚网卡」更常见的变化源。
std::vector<std::string> collect_mac_candidates(
    const std::vector<NetworkAdapterInfo>& adapters)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const NetworkAdapterInfo& a = adapters[i];
        if (a.is_loopback || !a.has_mac()) {
            continue;
        }
        if (looks_like_virtual_adapter(a.name) ||
            looks_like_virtual_adapter(a.description)) {
            continue;
        }
        const std::string mac = canonical(a.mac_address);
        if (mac.empty() ||
            std::find(out.begin(), out.end(), mac) != out.end()) {
            continue;
        }
        out.push_back(mac);
    }
    // 枚举顺序跨系统、跨重启都不保证；取字典序最小的那枚，结果因此确定
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> collect_disk_serials(
    const std::vector<DiskInfo>& disk_list)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < disk_list.size(); ++i) {
        const DiskInfo& d = disk_list[i];
        if (d.interface_type == "Virtual") {
            continue;  // 虚机 / 容器里透传的盘，标识每次重建都变
        }
        const std::string serial = canonical(d.serial_number);
        if (serial.empty() ||
            std::find(out.begin(), out.end(), serial) != out.end()) {
            continue;
        }
        out.push_back(serial);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool looks_virtual(const BiosInfo& bios,
                   const std::vector<NetworkAdapterInfo>& adapters)
{
    if (looks_like_virtual_machine(bios.system_vendor) ||
        looks_like_virtual_machine(bios.system_model) ||
        looks_like_virtual_machine(bios.bios_vendor)) {
        return true;
    }
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (contains_keyword(to_lower(adapters[i].name),
                             kHypervisorAdapterKeys) ||
            contains_keyword(to_lower(adapters[i].description),
                             kHypervisorAdapterKeys)) {
            return true;
        }
    }
    return false;
}

MachineFingerprint build_fingerprint(FingerprintPolicy policy,
                                     const std::string& salt)
{
    MachineFingerprint fp;
    fp.policy = policy;

    const CpuInfo cpu = cpu_info();
    const BiosInfo bios = bios_info();
    // 网卡无论策略如何都要取：MAC 信号之外还要用它判定是否虚拟化环境
    const std::vector<NetworkAdapterInfo> adapters = network_adapters();

    bool has_board = false;
    bool has_cpu = false;
    bool has_mac = false;
    bool has_disk = false;

    const std::string board = canonical(bios.serial_number);
    if (!board.empty()) {
        fp.signals.push_back("board=" + board);
        has_board = true;
    } else {
        fp.missing.push_back(
            "board=" + (bios.serial_error.empty()
                            ? std::string("序列号不可读或为占位值")
                            : bios.serial_error));
    }

    const std::string brand = canonical(cpu.brand);
    if (!brand.empty()) {
        fp.signals.push_back("cpu=" + brand);
        has_cpu = true;
    } else {
        fp.missing.push_back("cpu=" + (cpu.error.empty()
                                           ? std::string("型号不可读")
                                           : cpu.error));
    }

    if (policy != FingerprintPolicy::kStable) {
        const std::vector<std::string> macs = collect_mac_candidates(adapters);
        if (!macs.empty()) {
            fp.signals.push_back("mac=" + macs.front());
            has_mac = true;
        } else {
            fp.missing.push_back(
                "mac=无可用的物理网卡 MAC（回环与虚拟/隧道网卡已排除）");
        }
    }

    if (policy == FingerprintPolicy::kStrict) {
        const std::vector<std::string> serials = collect_disk_serials(disks());
        for (std::size_t i = 0; i < serials.size(); ++i) {
            fp.signals.push_back("disk=" + serials[i]);
        }
        has_disk = !serials.empty();
        if (!has_disk) {
            fp.missing.push_back(
                "disk=无可用的物理盘序列号（虚拟盘已排除，常见原因是缺读权限）");
        }
    }

    fp.is_virtual = looks_virtual(bios, adapters);

    if (has_board) {
        fp.source = "board";
    } else if (has_cpu) {
        fp.source = "cpu";
    } else if (has_mac) {
        fp.source = "mac";
    } else if (has_disk) {
        fp.source = "disk";
    }

    int conf = 0;
    if (has_board) {
        conf = has_cpu ? 90 : 80;
    } else if (has_cpu) {
        conf = 45;
    } else if (has_mac) {
        conf = 30;
    } else if (has_disk) {
        conf = 25;
    }
    if (has_mac) {
        conf += 5;
    }
    if (has_disk) {
        conf += 5;
    }
    if (conf > 100) {
        conf = 100;
    }
    if (fp.is_virtual && conf > 20) {
        // 虚机指纹不可用于授权：快照克隆会一指纹多机，容器重建会指纹突变
        conf = 20;
    }
    fp.confidence = conf;

    if (fp.signals.empty()) {
        // 一个可用信号都没有：id 留空、confidence 为 0，调用方据此判定不可用
        return fp;
    }

    std::string payload = std::string(kHashDomain) + "\n" +
                          kFingerprintVersion + "\n";
    if (!salt.empty()) {
        payload += "salt=" + salt + "\n";
    }
    for (std::size_t i = 0; i < fp.signals.size(); ++i) {
        payload += fp.signals[i] + "\n";
    }
    fp.id = Sha256::hex(payload);
    fp.short_id = fp.id.substr(0, 16);
    return fp;
}

}  // namespace

const char* machine_fingerprint_version()
{
    return kFingerprintVersion;
}

MachineFingerprint machine_fingerprint()
{
    return build_fingerprint(FingerprintPolicy::kBalanced, std::string());
}

MachineFingerprint machine_fingerprint_with(FingerprintPolicy policy,
                                           const std::string& salt)
{
    return build_fingerprint(policy, salt);
}

std::string canonical_fingerprint_token(const std::string& raw)
{
    return canonical(raw);
}

bool looks_like_virtual_machine(const std::string& description)
{
    const std::string lower = to_lower(description);
    if (lower.empty()) {
        return false;
    }
    return contains_keyword(lower, kMachineVendorKeys);
}

bool looks_like_virtual_adapter(const std::string& name_or_description)
{
    const std::string lower = to_lower(name_or_description);
    if (lower.empty()) {
        return false;
    }
    return contains_keyword(lower, kVirtualAdapterKeys);
}

bool is_virtual_machine()
{
    return looks_virtual(bios_info(), network_adapters());
}

}  // namespace libmini