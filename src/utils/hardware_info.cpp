#include "hardware_info.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <ntddscsi.h>
#include <winioctl.h>

#include "win_utf.h"
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#endif

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <nlohmann/json.hpp>
#endif

namespace libmini {

namespace {

// ==================== 通用小工具 ====================

std::string trim(const std::string& s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

// 十六进制字节串 → 小写无分隔形式
std::string hex_bytes(const unsigned char* data, std::size_t len)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

#if !defined(_WIN32)
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return s;
}

// 读整个文本文件；不存在/无权限返回 false
bool read_file(const std::string& path, std::string& out)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 从 "Capacity" 之类的文本解析字节数："1 TB" / "512 GB" / "8000 MB"。
// 解析失败返回 0（宁可留空也不给错值）。
std::uint64_t parse_capacity_text(const std::string& text)
{
    std::size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i >= text.size()) {
        return 0;
    }
    std::uint64_t value = 0;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) ||
                                text[i] == '.')) {
        if (text[i] != '.') {
            value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
        }
        ++i;
    }
    const std::string rest = lower(trim(text.substr(i)));
    std::uint64_t scale = 1;
    if (rest.rfind("tb", 0) == 0) {
        scale = 1000000000000ULL;
    } else if (rest.rfind("gb", 0) == 0) {
        scale = 1000000000ULL;
    } else if (rest.rfind("mb", 0) == 0) {
        scale = 1000000ULL;
    } else if (rest.rfind("kb", 0) == 0) {
        scale = 1000ULL;
    } else if (rest.rfind("b", 0) == 0) {
        scale = 1;
    }
    return value * scale;
}
#endif  // !_WIN32（下面两个辅助函数仅 POSIX 分支使用）

const char* architecture_name()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
    return "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

// 在 vector 里去重追加（适配器地址常因多 scope 重复）
template <typename T>
void append_unique(std::vector<T>& v, const T& value)
{
    if (std::find(v.begin(), v.end(), value) == v.end()) {
        v.push_back(value);
    }
}

#ifdef _WIN32

// 进程级 Winsock 引导（与 net_addr/tcp 各自的引导互不干扰，各自 call_once）
void platform_net_startup()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
        static struct Cleaner {
            ~Cleaner() { ::WSACleanup(); }
        } cleaner;
    });
}

// 读注册表字符串值（HKEY_LOCAL_MACHINE 下）。用宽字符版 API：REG_SZ 本质是
// UTF-16，A 版按当前 ANSI 代码页读会在中文机器上产生乱码
bool read_reg_string(const char* sub_key, const char* value_name,
                     std::string& out)
{
    std::wstring wsub = internal::utf8_to_wide(sub_key);
    std::wstring wname = internal::utf8_to_wide(value_name);
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, wsub.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[1024] = {0};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LONG rc = ::RegQueryValueExW(key, wname.c_str(), nullptr, &type,
                                        reinterpret_cast<LPBYTE>(buf), &size);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    out = trim(internal::wide_to_utf8(buf));
    return true;
}

// STORAGE_DEVICE_DESCRIPTOR 里的型号/序列号是系统 ANSI 代码页字节（无宽字符
// 变体），需按 CP_ACP 转成 UTF-8，否则中文型号乱码
std::string wide_from_ansi(const char* text)
{
    const int need = ::MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (need <= 1) {
        return std::string();
    }
    std::wstring wide(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], need);
    return internal::wide_to_utf8(wide.c_str(), static_cast<std::size_t>(need - 1));
}
std::string ansi_to_utf8(const char* text)
{
    if (text == nullptr || *text == '\0') {
        return std::string();
    }
    const std::size_t len = std::strlen(text);

    // 设备描述符的字符串编码因驱动而异（实测：真实硬件给系统 ANSI，部分虚拟
    // 盘驱动直接给 UTF-16LE），因此按三种策略依次尝试：
    //   1) 前若干字节里NUL 密集 → UTF-16LE
    //   2) 合法 UTF-8 → 原样
    //   3) 其余按 CP_ACP
    std::size_t probe_len = len < 8 ? len : 8;
    std::size_t nul_count = 0;
    for (std::size_t i = 0; i < probe_len; ++i) {
        if (text[i] == '\0') {
            ++nul_count;
        }
    }
    if (nul_count >= 2 && (len % 2) == 0) {
        std::wstring wide;
        wide.reserve(len / 2);
        for (std::size_t i = 0; i + 1 < len; i += 2) {
            const unsigned char lo = static_cast<unsigned char>(text[i]);
            const unsigned char hi = static_cast<unsigned char>(text[i + 1]);
            wide.push_back(static_cast<wchar_t>((hi << 8) | lo));
            if (wide.back() == 0) {
                break;
            }
        }
        return internal::wide_to_utf8(wide);
    }

    // 严格校验 UTF-8
    const char* p = text;
    while (*p != '\0') {
        const unsigned char lead = static_cast<unsigned char>(*p);
        int extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return wide_from_ansi(text);  // 非法 UTF-8 → 走 CP_ACP
        }
        ++p;
        for (int i = 0; i < extra; ++i) {
            const unsigned char cont = static_cast<unsigned char>(*p);
            if ((cont & 0xC0) != 0x80) {
                return wide_from_ansi(text);
            }
            ++p;
        }
    }
    return std::string(text);
}

// STORAGE_BUS_TYPE → 可读名
std::string bus_type_name(STORAGE_BUS_TYPE bus)
{
    switch (bus) {
        case BusTypeScsi:      return "SCSI";
        case BusTypeAtapi:     return "ATAPI";
        case BusTypeAta:       return "ATA";
        case BusType1394:      return "IEEE1394";
        case BusTypeSata:      return "SATA";
        case BusTypeSd:        return "SD";
        case BusTypeMmc:       return "MMC";
        case BusTypeVirtual:   return "Virtual";
        case BusTypeFileBackedVirtual: return "FileBackedVirtual";
        case BusTypeSpaces:    return "Spaces";
        case BusTypeNvme:      return "NVMe";
        case BusTypeSCM:       return "SCM";
        case BusTypeUfs:       return "UFS";
        default:               return "Unknown";
    }
}

// 枚举报错卷的 GUID 路径映射回盘符：QueryDosDeviceW("Volume{...}") 返回
// "C:\\" 之类的目标；映射不到（无盘符的分区）就保留原 GUID 路径
std::string volume_display_name(const wchar_t* volume_path)
{
    // 正确 API 是 GetVolumePathNamesForVolumeNameW——实测 QueryDosDeviceW 对卷
    // GUID 一律返回 0。它返回双 NUL 结尾的多字符串，取第一个即可。
    wchar_t paths[4096] = {0};
    DWORD length = 0;
    if (::GetVolumePathNamesForVolumeNameW(volume_path, paths, 4096,
                                           &length) &&
        paths[0] != 0) {
        std::wstring mount(paths);
        while (!mount.empty() && mount[mount.size() - 1] == 0x5C) {
            mount.erase(mount.size() - 1);  // "C:\" → "C:"
        }
        if (!mount.empty()) {
            return internal::wide_to_utf8(mount.c_str(), mount.size());
        }
    }
    // 无挂载点（无盘符的分区/系统保留卷）：退回 GUID 路径
    return internal::wide_to_utf8(volume_path);
}

// 从 STORAGE_DEVICE_DESCRIPTOR 的偏移字段取字符串
std::string descriptor_string(const STORAGE_DEVICE_DESCRIPTOR& desc,
                              DWORD offset)
{
    if (offset == 0 || offset >= desc.Size) {
        return std::string();
    }
    const char* base = reinterpret_cast<const char*>(&desc);
    return trim(ansi_to_utf8(base + offset));
}

#elif defined(__linux__)

// /proc/mounts 里的伪文件系统（不属于「卷」的语义）
bool is_pseudo_filesystem(const std::string& fs)
{
    static const char* kPseudo[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2",
        "securityfs", "pstore", "bpf", "debugfs", "tracefs", "configfs",
        "fusectl", "mqueue", "hugetlbfs", "autofs", "binfmt_misc", "ramfs",
        "squashfs", "efivarfs", "nsfs", "rpc_pipefs", "selinuxfs"
    };
    for (std::size_t i = 0; i < sizeof(kPseudo) / sizeof(kPseudo[0]); ++i) {
        if (fs == kPseudo[i]) {
            return true;
        }
    }
    return false;
}

// 虚拟/分区设备：跳过 loop、ram、zram、sr 光驱、dm 映射、md raid
bool is_virtual_block_name(const std::string& name)
{
    static const char* kPrefixes[] = { "loop", "ram", "zram", "sr", "dm-", "md", "fd" };
    for (std::size_t i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); ++i) {
        if (name.rfind(kPrefixes[i], 0) == 0) {
            return true;
        }
    }
    return false;
}

// 从 /dev/disk/by-uuid 建立「真实设备路径 → 文件系统 UUID」映射，
// 用来给卷填一个无需 root 就能拿到的标识（Linux 没有可移植的卷序列号）。
void load_uuid_map(std::vector<std::pair<std::string, std::string> >& out)
{
    DIR* dir = ::opendir("/dev/disk/by-uuid");
    if (!dir) {
        return;
    }
    char resolved[PATH_MAX];
    while (struct dirent* ent = ::readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string link = std::string("/dev/disk/by-uuid/") + name;
        if (::realpath(link.c_str(), resolved) == nullptr) {
            continue;
        }
        out.push_back(std::make_pair(std::string(resolved), name));
    }
    ::closedir(dir);
}

#endif  // 平台小工具

}  // namespace

// ==================== 公共 API ====================

std::string format_mac(const std::string& raw)
{
    std::string hex;
    hex.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        } else if (ch != '-' && ch != ':' && ch != '.' && ch != ' ') {
            return raw;  // 含非法字符：原样返回，不猜测
        }
    }
    if (hex.size() != 12) {
        return raw;
    }
    std::string out;
    out.reserve(17);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        if (!out.empty()) {
            out.push_back(':');
        }
        out.push_back(hex[i]);
        out.push_back(hex[i + 1]);
    }
    return out;
}

// ------------------------------ CPU ------------------------------

CpuInfo cpu_info()
{
    CpuInfo info;
    info.architecture = architecture_name();

#ifdef _WIN32
    // 品牌/厂商/频率：注册表 CentralProcessor\0
    static const char* kCpuKey =
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    if (!read_reg_string(kCpuKey, "ProcessorNameString", info.brand)) {
        info.error = "注册表无 ProcessorNameString（受限环境或非 x86 平台）";
    }
    (void)read_reg_string(kCpuKey, "VendorIdentifier", info.vendor);

    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    DWORD type = 0;
    HKEY key = nullptr;
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, kCpuKey, 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        if (::RegQueryValueExA(key, "~MHz", nullptr, &type,
                               reinterpret_cast<LPBYTE>(&mhz),
                               &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            info.max_frequency_mhz = static_cast<int>(mhz);
        }
        ::RegCloseKey(key);
    }

    // 逻辑核：优先 GetActiveProcessorCount（不受 64 核组上限影响）
    DWORD active = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active == 0) {
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        active = si.dwNumberOfProcessors;
    }
    info.logical_cores = static_cast<int>(active);

    // 物理核：RelationProcessorCore 记录数
    DWORD len = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        // 用 DWORD 数组保证对齐
        std::vector<DWORD> storage((len / sizeof(DWORD)) + 2, 0);
        if (::GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    storage.data()),
                &len)) {
            int cores = 0;
            std::size_t offset = 0;
            while (offset + sizeof(DWORD) <= len) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec =
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        storage.data() + offset);
                if (rec->Size == 0) {
                    break;
                }
                if (rec->Relationship == RelationProcessorCore) {
                    ++cores;
                }
                offset += rec->Size;
            }
            if (cores > 0) {
                info.physical_cores = cores;
            }
        }
    }

#elif defined(__linux__)
    std::ifstream in("/proc/cpuinfo");
    if (in) {
        std::string line;
        // 物理核 = 去重后的 (physical id, core id) 组合数
        std::vector<std::pair<int, int> > core_pairs;
        int physical_id = -1;
        int core_id = -1;
        int cores_per_socket = 0;
        while (std::getline(in, line)) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = trim(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            if (key == "model name" && info.brand.empty()) {
                info.brand = value;
            } else if (key == "vendor_id" && info.vendor.empty()) {
                info.vendor = value;
            } else if (key == "cpu MHz") {
                const double mhz = std::atof(value.c_str());
                if (mhz > 0 && static_cast<int>(mhz) > info.max_frequency_mhz) {
                    info.max_frequency_mhz = static_cast<int>(mhz);
                }
            } else if (key == "processor") {
                ++info.logical_cores;
                // 上一条 processor 记录在此结算
                if (physical_id >= 0 && core_id >= 0) {
                    const std::pair<int, int> pair(physical_id, core_id);
                    if (std::find(core_pairs.begin(), core_pairs.end(), pair) ==
                        core_pairs.end()) {
                        core_pairs.push_back(pair);
                    }
                }
                physical_id = -1;
                core_id = -1;
            } else if (key == "physical id") {
                physical_id = std::atoi(value.c_str());
            } else if (key == "core id") {
                core_id = std::atoi(value.c_str());
            } else if (key == "cpu cores") {
                cores_per_socket = std::atoi(value.c_str());
            }
        }
        // 末条记录结算
        if (physical_id >= 0 && core_id >= 0) {
            const std::pair<int, int> pair(physical_id, core_id);
            if (std::find(core_pairs.begin(), core_pairs.end(), pair) ==
                core_pairs.end()) {
                core_pairs.push_back(pair);
            }
        }
        if (!core_pairs.empty()) {
            info.physical_cores = static_cast<int>(core_pairs.size());
        } else if (cores_per_socket > 0) {
            // 无 physical id/core id 的精简内核：cpu cores 即物理核
            info.physical_cores = cores_per_socket;
        }
    }
    if (info.brand.empty()) {
        info.error = "/proc/cpuinfo 不可读（容器或非标准内核）";
    }

#elif defined(__APPLE__)
    char brand[256] = {0};
    std::size_t len = sizeof(brand);
    if (::sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) ==
        0) {
        info.brand = trim(brand);
    } else {
        info.error = "sysctl machdep.cpu.brand_string 不可用";
    }
    std::uint64_t physical = 0;
    std::size_t vlen = sizeof(physical);
    if (::sysctlbyname("hw.physicalcpu", &physical, &vlen, nullptr, 0) == 0) {
        info.physical_cores = static_cast<int>(physical);
    }
    vlen = sizeof(physical);
    if (::sysctlbyname("hw.logicalcpu", &physical, &vlen, nullptr, 0) == 0) {
        info.logical_cores = static_cast<int>(physical);
    }
    // 标称频率：Intel 有 hw.cpufrequency，Apple Silicon 无此键
    std::uint64_t freq = 0;
    vlen = sizeof(freq);
    if (::sysctlbyname("hw.cpufrequency", &freq, &vlen, nullptr, 0) == 0) {
        info.max_frequency_mhz = static_cast<int>(freq / 1000000ULL);
    }
    if (info.vendor.empty()) {
        info.vendor = "Apple";
    }

#else
    info.error = "当前平台未实现 CPU 型号查询";
#endif

    if (info.physical_cores > 0 && info.logical_cores > info.physical_cores) {
        info.hyperthreading = true;
    }
    return info;
}

// ------------------------------ 网络适配器 ------------------------------

std::vector<NetworkAdapterInfo> network_adapters()
{
    std::vector<NetworkAdapterInfo> out;

#ifdef _WIN32
    platform_net_startup();

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = ::GetAdaptersAddresses(
        AF_UNSPEC, flags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = ::GetAdaptersAddresses(
            AF_UNSPEC, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc == NO_ERROR) {
        IP_ADAPTER_ADDRESSES* adapters =
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        for (IP_ADAPTER_ADDRESSES* a = adapters; a != nullptr; a = a->Next) {
            NetworkAdapterInfo n;
            n.name = internal::wide_to_utf8(a->FriendlyName);
            if (n.name.empty() && a->AdapterName != nullptr) {
                n.name = std::string(a->AdapterName);  // ASCII GUID 字符串
            }
            n.description = internal::wide_to_utf8(a->Description);
            n.is_loopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
            n.is_up = (a->OperStatus == IfOperStatusUp);

            if (a->PhysicalAddressLength > 0 &&
                a->PhysicalAddressLength <= 32) {
                const std::string hex = hex_bytes(a->PhysicalAddress,
                                                   a->PhysicalAddressLength);
                n.mac_address = format_mac(hex);
            } else {
                n.mac_error = "接口不提供 MAC（虚拟/隧道适配器）";
            }

            for (IP_ADAPTER_UNICAST_ADDRESS* ua = a->FirstUnicastAddress;
                 ua != nullptr; ua = ua->Next) {
                if (ua->Address.lpSockaddr == nullptr) {
                    continue;
                }
                char text[INET6_ADDRSTRLEN] = {0};
                if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                    sockaddr_in* v4 =
                        reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                    // 跳过 0.0.0.0（未指定地址）
                    if (v4->sin_addr.s_addr == 0) {
                        continue;
                    }
                    if (::inet_ntop(AF_INET, &v4->sin_addr, text,
                                    sizeof(text)) != nullptr) {
                        append_unique(n.ipv4_addresses, std::string(text));
                    }
                } else if (ua->Address.lpSockaddr->sa_family == AF_INET6) {
                    sockaddr_in6* v6 = reinterpret_cast<sockaddr_in6*>(
                        ua->Address.lpSockaddr);
                    if (IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr)) {
                        continue;
                    }
                    if (::inet_ntop(AF_INET6, &v6->sin6_addr, text,
                                    sizeof(text)) != nullptr) {
                        append_unique(n.ipv6_addresses, std::string(text));
                    }
                }
            }
            out.push_back(n);
        }
    }

#elif defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    // Linux / macOS：getifaddrs 两遍扫描——第一遍收 IP 与 flags，第二遍收 MAC
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) == 0) {
        std::vector<std::size_t> order;  // 保持首次出现顺序
        for (struct ifaddrs* ifa = ifs; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name == nullptr || ifa->ifa_addr == nullptr) {
                continue;
            }
            const std::string name = ifa->ifa_name;
            std::size_t index = out.size();
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (out[i].name == name) {
                    index = i;
                    break;
                }
            }
            if (index == out.size()) {
                NetworkAdapterInfo n;
                n.name = name;
                out.push_back(n);
            }
            NetworkAdapterInfo& n = out[index];

            const unsigned int flags = ifa->ifa_flags;
            if ((flags & IFF_UP) != 0) {
                n.is_up = true;
            }
            if ((flags & IFF_LOOPBACK) != 0 || name == "lo" || name == "lo0") {
                n.is_loopback = true;
            }

            char text[INET6_ADDRSTRLEN] = {0};
            if (ifa->ifa_addr->sa_family == AF_INET) {
                sockaddr_in* v4 = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                if (v4->sin_addr.s_addr != 0 &&
                    ::inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)) !=
                        nullptr) {
                    append_unique(n.ipv4_addresses, std::string(text));
                }
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                sockaddr_in6* v6 =
                    reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
                if (!IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr) &&
                    ::inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text)) !=
                        nullptr) {
                    append_unique(n.ipv6_addresses, std::string(text));
                }
            }
        }
        // 第二遍：MAC
        for (struct ifaddrs* ifa = ifs; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name == nullptr || ifa->ifa_addr == nullptr) {
                continue;
            }
            const std::string name = ifa->ifa_name;
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (out[i].name != name) {
                    continue;
                }
                if (!out[i].mac_address.empty()) {
                    break;
                }
#ifdef __APPLE__
                if (ifa->ifa_addr->sa_family == AF_LINK) {
                    sockaddr_dl* sdl =
                        reinterpret_cast<sockaddr_dl*>(ifa->ifa_addr);
                    if (sdl->sdl_alen > 0 && sdl->sdl_alen <= 32) {
                        out[i].mac_address = format_mac(
                            hex_bytes(LL_ADDR(sdl), sdl->sdl_alen));
                    }
                }
#else
                if (ifa->ifa_addr->sa_family == AF_PACKET) {
                    sockaddr_ll* sll =
                        reinterpret_cast<sockaddr_ll*>(ifa->ifa_addr);
                    if (sll->sll_halen > 0 && sll->sll_halen <= 32) {
                        out[i].mac_address = format_mac(
                            hex_bytes(sll->sll_addr, sll->sll_halen));
                    }
                }
#endif
                break;
            }
        }
        // 无 MAC 的适配器给出原因
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i].mac_address.empty() && out[i].mac_error.empty()) {
                out[i].mac_error =
                    out[i].is_loopback ? "回环接口无 MAC 地址"
                                       : "接口未提供 MAC 地址";
            }
        }
        ::freeifaddrs(ifs);
    }
#else
    (void)out;
#endif

    return out;
}

std::string primary_mac_address()
{
    const std::vector<NetworkAdapterInfo> adapters = network_adapters();
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (!adapters[i].is_loopback && adapters[i].is_up &&
            adapters[i].has_mac()) {
            return adapters[i].mac_address;
        }
    }
    return std::string();
}

std::string primary_ipv4_address()
{
    const std::vector<NetworkAdapterInfo> adapters = network_adapters();
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (!adapters[i].is_loopback && adapters[i].is_up &&
            !adapters[i].ipv4_addresses.empty()) {
            return adapters[i].ipv4_addresses.front();
        }
    }
    return std::string();
}

// ------------------------------ 物理磁盘 ------------------------------

std::vector<DiskInfo> disks()
{
    std::vector<DiskInfo> out;

#ifdef _WIN32
    // 逐号打开 \\.\PhysicalDriveN：以能打开为准，编号有洞也没关系，
    // 比走 SetupAPI 枚举设备实例更直接、失败更少
    for (int index = 0; index < 64; ++index) {
        wchar_t path[64] = {0};
        ::swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", index);

        // 权限逐级尝试：读权限能拿到容量（IOCTL_DISK_GET_LENGTH_INFO 需要），
        // 拿不到读权限时退到 0 权限（虚拟盘通常只给这个）
        HANDLE handle = ::CreateFileW(path, GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
        bool with_read_access = (handle != INVALID_HANDLE_VALUE);
        if (!with_read_access) {
            handle = ::CreateFileW(path, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            continue;  // 不存在的编号 / 完全无权限，均跳过
        }

        DiskInfo disk;
        disk.device_path = internal::wide_to_utf8(path);

        STORAGE_PROPERTY_QUERY query;
        ::memset(&query, 0, sizeof(query));
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        STORAGE_DEVICE_DESCRIPTOR desc;
        ::memset(&desc, 0, sizeof(desc));
        DWORD returned = 0;
        if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query,
                              sizeof(query), &desc, sizeof(desc), &returned,
                              nullptr)) {
            disk.model = descriptor_string(desc, desc.ProductIdOffset);
            disk.serial_number = descriptor_string(desc, desc.SerialNumberOffset);
            disk.interface_type = bus_type_name(desc.BusType);
            disk.is_removable = (desc.RemovableMedia != 0);
            if (disk.serial_number.empty()) {
                disk.serial_error = "驱动未提供序列号（虚拟盘/RAID 常见）";
            }
        } else {
            disk.interface_type = "Unknown";
            disk.serial_error = "设备描述查询失败（需管理员权限或为虚拟盘）";
        }

        GET_LENGTH_INFORMATION length_info;
        ::memset(&length_info, 0, sizeof(length_info));
        bool got_size = ::DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO,
                                          nullptr, 0, &length_info,
                                          sizeof(length_info), &returned,
                                          nullptr) != 0;
        ::CloseHandle(handle);

        // 0 权限拿不到容量时，用读权限重开一次（部分驱动区分这两种权限）
        if (!got_size && !with_read_access) {
            HANDLE retry = ::CreateFileW(path, GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, 0, nullptr);
            if (retry != INVALID_HANDLE_VALUE) {
                ::memset(&length_info, 0, sizeof(length_info));
                got_size = ::DeviceIoControl(retry,
                                              IOCTL_DISK_GET_LENGTH_INFO,
                                              nullptr, 0, &length_info,
                                              sizeof(length_info), &returned,
                                              nullptr) != 0;
                ::CloseHandle(retry);
            }
        }
        if (got_size) {
            disk.size_bytes = static_cast<std::uint64_t>(
                length_info.Length.QuadPart);
        }
        out.push_back(disk);
    }

#elif defined(__linux__)
    DIR* dir = ::opendir("/sys/class/block");
    if (!dir) {
        return out;
    }
    while (struct dirent* ent = ::readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == ".." || is_virtual_block_name(name)) {
            continue;
        }
        const std::string base = "/sys/class/block/" + name;
        // 分区没有 device 链接——以它是否存在过滤掉分区
        std::string device_link = base + "/device";
        char resolved[PATH_MAX];
        const bool has_device =
            ::realpath(device_link.c_str(), resolved) != nullptr;
        if (!has_device) {
            continue;
        }
        const std::string device_real(resolved);

        DiskInfo disk;
        disk.device_path = "/dev/" + name;

        std::string text;
        if (read_file(base + "/size", text)) {
            // /sys 的 size 单位固定为 512 字节扇区
            disk.size_bytes =
                static_cast<std::uint64_t>(std::atoll(trim(text).c_str())) * 512ULL;
        }
        if (read_file(base + "/removable", text) && trim(text) == "1") {
            disk.is_removable = true;
        }
        if (read_file(base + "/device/model", text)) {
            disk.model = trim(text);
        }
        if (read_file(base + "/device/serial", text)) {
            disk.serial_number = trim(text);
        }
        if (disk.serial_number.empty()) {
            disk.serial_error =
                "无序列号或需 root（虚拟盘、USB 读卡器、部分 RAID 常见）";
        }

        // 总线类型：看 device 符号链接指向的真实路径
        if (device_real.find("/nvme/") != std::string::npos) {
            disk.interface_type = "NVMe";
        } else if (device_real.find("usb") != std::string::npos) {
            disk.interface_type = "USB";
        } else if (device_real.find("/ata") != std::string::npos ||
                   device_real.find("host") != std::string::npos) {
            disk.interface_type = "SATA";
        } else {
            disk.interface_type = "Unknown";
        }
        out.push_back(disk);
    }
    ::closedir(dir);

#elif defined(__APPLE__)
    // macOS 无公开的块设备枚举 API（IOKit 过于底层）；走 system_profiler
    // 的 JSON 输出，无需 root。字段缺失/格式变化时返回空列表并让调用方
    // 通过“无结果”得知不可用。
    std::string output;
    FILE* pipe = ::popen(
        "system_profiler -json SPNVMeDataType SPSerialATADataType 2>/dev/null",
        "r");
    if (pipe) {
        char chunk[4096];
        while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) {
            output += chunk;
        }
        ::pclose(pipe);
    }
    if (!output.empty()) {
        nlohmann::json parsed = nlohmann::json::parse(output, nullptr, false);
        if (!parsed.is_discarded()) {
            const char* sections[2] = { "SPNVMeDataType", "SPSerialATADataType" };
            for (int s = 0; s < 2; ++s) {
                if (!parsed.contains(sections[s]) || !parsed[sections[s]].is_array()) {
                    continue;
                }
                for (const nlohmann::json& item : parsed[sections[s]]) {
                    DiskInfo disk;
                    const std::string name =
                        item.value("_name", std::string());
                    disk.device_path = name;
                    disk.model = item.value("Model", std::string());
                    if (disk.model.empty()) {
                        disk.model = name;
                    }
                    disk.serial_number = item.value("Serial Number", std::string());
                    if (disk.serial_number.empty() ||
                        disk.serial_number == "null") {
                        disk.serial_number.clear();
                        disk.serial_error = "system_profiler 未提供序列号";
                    }
                    disk.size_bytes = parse_capacity_text(
                        item.value("Capacity", std::string()));
                    disk.interface_type =
                        (s == 0) ? "NVMe" : "SATA";
                    if (!disk.device_path.empty() || !disk.model.empty()) {
                        out.push_back(disk);
                    }
                }
            }
        }
    }
#endif

    return out;
}

// ------------------------------ 卷 ------------------------------

std::vector<VolumeInfo> volumes()
{
    std::vector<VolumeInfo> out;

#ifdef _WIN32
    wchar_t volume_name[MAX_PATH] = {0};
    HANDLE search = ::FindFirstVolumeW(volume_name, MAX_PATH);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            VolumeInfo vol;
            vol.mount_point = volume_display_name(volume_name);

            wchar_t label[MAX_PATH + 1] = {0};
            wchar_t fs[MAX_PATH + 1] = {0};
            DWORD serial = 0;
            DWORD max_component = 0;
            DWORD flags = 0;
            if (::GetVolumeInformationW(volume_name, label, MAX_PATH, &serial,
                                         &max_component, &flags, fs, MAX_PATH)) {
                vol.label = internal::wide_to_utf8(label);
                vol.filesystem = internal::wide_to_utf8(fs);
                char hex[16] = {0};
                ::snprintf(hex, sizeof(hex), "%08X",
                           static_cast<unsigned int>(serial));
                vol.serial_number = hex;
            } else {
                vol.serial_error = "卷信息查询失败（盘未就绪或无权限）";
            }

            ULARGE_INTEGER available = {};
            ULARGE_INTEGER total = {};
            ULARGE_INTEGER free_bytes = {};
            if (::GetDiskFreeSpaceExW(volume_name, &available, &total,
                                      &free_bytes)) {
                vol.total_bytes = static_cast<std::uint64_t>(total.QuadPart);
                vol.free_bytes = static_cast<std::uint64_t>(free_bytes.QuadPart);
            }
            // 空读卡器/未挂载的光驱：跳过，避免噪声
            if (vol.total_bytes > 0) {
                out.push_back(vol);
            }
        } while (::FindNextVolumeW(search, volume_name, MAX_PATH));
        ::FindVolumeClose(search);
    }

#elif defined(__linux__)
    std::vector<std::pair<std::string, std::string> > uuid_map;
    load_uuid_map(uuid_map);

    std::ifstream mounts("/proc/mounts");
    if (mounts) {
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream fields(line);
            std::string device;
            std::string mount_point;
            std::string fs;
            if (!(fields >> device >> mount_point >> fs)) {
                continue;
            }
            if (is_pseudo_filesystem(fs)) {
                continue;
            }
            VolumeInfo vol;
            vol.mount_point = mount_point;
            vol.filesystem = fs;

            struct statvfs vfs;
            if (::statvfs(mount_point.c_str(), &vfs) == 0) {
                vol.total_bytes = static_cast<std::uint64_t>(vfs.f_blocks) *
                                  static_cast<std::uint64_t>(vfs.f_frsize);
                vol.free_bytes = static_cast<std::uint64_t>(vfs.f_bavail) *
                                 static_cast<std::uint64_t>(vfs.f_frsize);
            }
            // 标识用文件系统 UUID（Linux 无可移植的“卷序列号”）
            char resolved[PATH_MAX];
            const std::string real =
                (::realpath(device.c_str(), resolved) != nullptr)
                    ? std::string(resolved)
                    : device;
            for (std::size_t i = 0; i < uuid_map.size(); ++i) {
                if (uuid_map[i].first == real) {
                    vol.serial_number = uuid_map[i].second;
                    break;
                }
            }
            if (vol.serial_number.empty()) {
                vol.serial_error = "无 /dev/disk/by-uuid 条目（UUID 即文件系统标识）";
            }
            out.push_back(vol);
        }
    }

#elif defined(__APPLE__)
    struct statfs* mounts = nullptr;
    const int count = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (mounts != nullptr && count > 0) {
        for (int i = 0; i < count; ++i) {
            VolumeInfo vol;
            vol.mount_point = mounts[i].f_mntonname;
            vol.filesystem = mounts[i].f_fstypename;
            vol.total_bytes = static_cast<std::uint64_t>(mounts[i].f_blocks) *
                              static_cast<std::uint64_t>(mounts[i].f_bsize);
            vol.free_bytes = static_cast<std::uint64_t>(mounts[i].f_bavail) *
                             static_cast<std::uint64_t>(mounts[i].f_bsize);
            vol.serial_error = "macOS 未公开卷序列号接口（需 diskutil 提权）";
            out.push_back(vol);
        }
    }
#endif

    return out;
}

// ------------------------------ 主板 / BIOS ------------------------------

BiosInfo bios_info()
{
    BiosInfo info;

#ifdef _WIN32
    static const char* kBiosKey = "HARDWARE\\DESCRIPTION\\System\\BIOS";
    (void)read_reg_string(kBiosKey, "SystemManufacturer", info.system_vendor);
    (void)read_reg_string(kBiosKey, "SystemProductName", info.system_model);
    (void)read_reg_string(kBiosKey, "BIOSVendor", info.bios_vendor);
    (void)read_reg_string(kBiosKey, "BIOSVersion", info.bios_version);
    (void)read_reg_string(kBiosKey, "BIOSReleaseDate", info.bios_release_date);

    // 序列号在 BIOS 键的子键下（不同厂商位置不同，逐个子键找）
    HKEY bios = nullptr;
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, kBiosKey, 0, KEY_READ, &bios) ==
        ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            char sub_name[256] = {0};
            DWORD sub_len = sizeof(sub_name);
            if (::RegEnumKeyExA(bios, i, sub_name, &sub_len, nullptr, nullptr,
                                nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            std::string full = std::string(kBiosKey) + "\\" + sub_name;
            static const char* kValueNames[3] = { "SystemSerialNumber",
                                                  "BIOSSerialNumber",
                                                  "BaseBoardSerialNumber" };
            for (int v = 0; v < 3; ++v) {
                if (info.serial_number.empty()) {
                    (void)read_reg_string(full.c_str(), kValueNames[v],
                                          info.serial_number);
                }
            }
            // 厂商自定义命名（如 HP 的 "HPBIOSSystemSerialNumber"）：扫一遍
            // 子键里名字含 Serial 的字符串值作为兜底
            if (info.serial_number.empty()) {
                HKEY sub = nullptr;
                if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, full.c_str(), 0,
                                    KEY_READ, &sub) == ERROR_SUCCESS) {
                    for (DWORD v = 0;; ++v) {
                        char name[256] = {0};
                        DWORD name_len = sizeof(name);
                        if (::RegEnumValueA(sub, v, name, &name_len, nullptr,
                                            nullptr, nullptr,
                                            nullptr) != ERROR_SUCCESS) {
                            break;
                        }
                        const std::string value_name(name);
                        if (value_name.find("Serial") == std::string::npos) {
                            continue;
                        }
                        if (read_reg_string(full.c_str(),
                                            value_name.c_str(),
                                            info.serial_number)) {
                            break;
                        }
                    }
                    ::RegCloseKey(sub);
                }
            }
            if (!info.serial_number.empty()) {
                break;
            }
        }
        ::RegCloseKey(bios);
    }
    if (info.serial_number.empty()) {
        info.serial_error = "BIOS 序列号未在注册表暴露（虚拟机常见）";
    }

#elif defined(__linux__)
    static const char* kDmi = "/sys/class/dmi/id/";
    std::string text;
    if (read_file(std::string(kDmi) + "sys_vendor", text)) {
        info.system_vendor = trim(text);
    }
    if (read_file(std::string(kDmi) + "product_name", text)) {
        info.system_model = trim(text);
    }
    if (read_file(std::string(kDmi) + "bios_vendor", text)) {
        info.bios_vendor = trim(text);
    }
    if (read_file(std::string(kDmi) + "bios_version", text)) {
        info.bios_version = trim(text);
    }
    if (read_file(std::string(kDmi) + "bios_date", text)) {
        info.bios_release_date = trim(text);
    }
    if (read_file(std::string(kDmi) + "product_serial", text)) {
        info.serial_number = trim(text);
    }
    if (info.serial_number.empty()) {
        info.serial_error = "product_serial 需 root（内核已做权限限制）";
    }

#elif defined(__APPLE__)
    std::string output;
    FILE* pipe = ::popen("system_profiler -json SPHardwareDataType 2>/dev/null", "r");
    if (pipe) {
        char chunk[4096];
        while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) {
            output += chunk;
        }
        ::pclose(pipe);
    }
    if (!output.empty()) {
        nlohmann::json parsed = nlohmann::json::parse(output, nullptr, false);
        if (!parsed.is_discarded() && parsed.contains("SPHardwareDataType")) {
            const nlohmann::json& hw = parsed["SPHardwareDataType"];
            if (hw.is_array() && !hw.empty()) {
                const nlohmann::json& item = hw[0];
                info.system_vendor = item.value("Hardware Model", std::string());
                info.system_model = item.value("Machine Model", std::string());
                info.bios_version = item.value("ROM Version", std::string());
                info.serial_number = item.value("Serial Number", std::string());
            }
        }
    }
    if (info.serial_number.empty() || info.serial_number == "null") {
        info.serial_number.clear();
        info.serial_error = "system_profiler 未提供序列号";
    }
#endif

    return info;
}

}  // namespace libmini