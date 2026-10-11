#include "system_info.h"

#include <mutex>

#include "win_utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
// macOS 没有 sys/sysinfo.h，内存信息走 sysctl（hw.memsize）
#include <climits>
#include <cstdio>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__FreeBSD__)
#include <climits>
#include <cstdio>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>
#else
#include <climits>
#include <cstdio>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace libmini {

std::string hostname()
{
#ifdef _WIN32
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!::GetComputerNameW(buf, &size)) {
        return std::string();
    }
    return internal::wide_to_utf8(std::wstring(buf, size));
#else
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) {
        return std::string();
    }
    return std::string(buf);
#endif
}

std::uint32_t current_pid()
{
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::string executable_path()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::string();
    }
    return internal::wide_to_utf8(std::wstring(buf, n));
#else
    char buf[4096];
#if defined(__APPLE__) || defined(__FreeBSD__)
    // macOS/BSD 无 /proc：走 proc_pidpath/sysctl KERN_PROCARGS；
    // 通用兜底 procstat 在此不引入，保持零依赖
#if defined(__APPLE__)
    char path_buf[PATH_MAX];
    if (proc_pidpath(::getpid(), path_buf, sizeof(path_buf)) <= 0) {
        return std::string();
    }
    return std::string(path_buf);
#else  // __FreeBSD__
    const int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    std::size_t len = sizeof(buf) - 1;
    if (::sysctl(const_cast<int*>(mib), 4, buf, &len, NULL, 0) != 0 ||
        len == 0) {
        return std::string();
    }
    return std::string(buf, len > 0 && buf[len - 1] == '\0' ? len - 1 : len);
#endif
#else
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf, static_cast<std::size_t>(n));
#endif
#endif
}

int cpu_count()
{
#ifdef _WIN32
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    return static_cast<int>(si.dwNumberOfProcessors);
#else
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 0;
#endif
}

std::uint64_t total_physical_memory()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!::GlobalMemoryStatusEx(&ms)) {
        return 0;
    }
    return static_cast<std::uint64_t>(ms.ullTotalPhys);
#elif defined(__APPLE__)
    std::uint64_t mem = 0;
    std::size_t len = sizeof(mem);
    if (::sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) {
        return 0;
    }
    return mem;
#else
    struct ::sysinfo si;
    if (::sysinfo(&si) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(si.totalram) *
           static_cast<std::uint64_t>(si.mem_unit);
#endif
}

std::uint64_t available_physical_memory()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!::GlobalMemoryStatusEx(&ms)) {
        return 0;
    }
    return static_cast<std::uint64_t>(ms.ullAvailPhys);
#elif defined(__APPLE__)
    // macOS 无 sysinfo()：以空闲页数 × 页大小近似（不含可回收缓存，
    // 口径偏保守，仅作展示用途）
    std::uint64_t page_size = 4096;
    std::size_t plen = sizeof(page_size);
    ::sysctlbyname("hw.pagesize", &page_size, &plen, NULL, 0);
    std::uint64_t free_pages = 0;
    std::size_t flen = sizeof(free_pages);
    if (::sysctlbyname("vm.page_free_count", &free_pages, &flen, NULL, 0) !=
        0) {
        return 0;
    }
    return free_pages * page_size;
#else
    struct ::sysinfo si;
    if (::sysinfo(&si) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(si.freeram) *
           static_cast<std::uint64_t>(si.mem_unit);
#endif
}

std::uint64_t disk_total_bytes(const std::string& path)
{
#ifdef _WIN32
    ULARGE_INTEGER total = {};
    if (!::GetDiskFreeSpaceExW(internal::utf8_to_wide(path).c_str(), NULL,
                               &total, NULL)) {
        return 0;
    }
    return static_cast<std::uint64_t>(total.QuadPart);
#else
    struct ::statvfs vfs;
    if (::statvfs(path.c_str(), &vfs) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(vfs.f_blocks) *
           static_cast<std::uint64_t>(vfs.f_frsize);
#endif
}

std::uint64_t disk_free_bytes(const std::string& path)
{
#ifdef _WIN32
    ULARGE_INTEGER free_bytes = {};
    if (!::GetDiskFreeSpaceExW(internal::utf8_to_wide(path).c_str(), NULL,
                               NULL, &free_bytes)) {
        return 0;
    }
    return static_cast<std::uint64_t>(free_bytes.QuadPart);
#else
    struct ::statvfs vfs;
    if (::statvfs(path.c_str(), &vfs) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(vfs.f_bavail) *
           static_cast<std::uint64_t>(vfs.f_frsize);
#endif
}

namespace {

// CPU 累计时间。单位在各平台之间不同，但**同一平台内一致**，而
// cpu_usage_percent 只用两次采样之间的差值比例——单位在比值里约掉，
// 因此这里不做刻意的单位换算。
struct CpuTicks
{
    bool valid = false;
    std::uint64_t total = 0;
    std::uint64_t idle = 0;
};

std::mutex& cpu_baseline_mutex()
{
    static std::mutex mutex;
    return mutex;
}

CpuTicks& cpu_baseline()
{
    static CpuTicks ticks;
    return ticks;
}

bool read_cpu_ticks(CpuTicks* out)
{
#ifdef _WIN32
    FILETIME idle_time;
    FILETIME kernel_time;
    FILETIME user_time;
    if (!::GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return false;
    }
    ULARGE_INTEGER idle;
    ULARGE_INTEGER kernel;
    ULARGE_INTEGER user;
    idle.LowPart = idle_time.dwLowDateTime;
    idle.HighPart = idle_time.dwHighDateTime;
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    out->idle = static_cast<std::uint64_t>(idle.QuadPart);
    // kernel 已经包含 idle，所以总时间 = kernel + user
    out->total = static_cast<std::uint64_t>(kernel.QuadPart) +
                 static_cast<std::uint64_t>(user.QuadPart);
    out->valid = true;
    return true;
#elif defined(__APPLE__)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (::host_statistics(::mach_host_self(), HOST_CPU_LOAD_INFO,
                          reinterpret_cast<host_info_t>(&info), &count) != KERN_SUCCESS) {
        return false;
    }
    const std::uint64_t user = info.cpu_ticks[CPU_STATE_USER];
    const std::uint64_t nice = info.cpu_ticks[CPU_STATE_NICE];
    const std::uint64_t system = info.cpu_ticks[CPU_STATE_SYSTEM];
    const std::uint64_t idle = info.cpu_ticks[CPU_STATE_IDLE];
    out->idle = idle;
    out->total = user + nice + system + idle;
    out->valid = true;
    return true;
#elif defined(__FreeBSD__)
    // FreeBSD 需要 sysctl(CTL_KERN, KERN_CP_TIME) 才能拿到聚合值，本模块暂不
    // 引入；调用方拿到 -1（与「读取失败」同一语义）。
    (void)out;
    return false;
#else
    // Linux 等：/proc/stat 首行的 cpu 汇总
    FILE* file = std::fopen("/proc/stat", "r");
    if (file == NULL) {
        return false;
    }
    char label[16] = {0};
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
    const int fields = std::fscanf(file, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                                   label, &user, &nice, &system, &idle, &iowait, &irq,
                                   &softirq, &steal);
    std::fclose(file);
    if (fields < 5) {
        return false;
    }
    out->idle = idle + iowait;  // iowait 也算「非忙」
    out->total = user + nice + system + idle + iowait + irq + softirq + steal;
    out->valid = true;
    return true;
#endif
}

}  // namespace

double cpu_usage_percent()
{
    CpuTicks current;
    if (!read_cpu_ticks(&current)) {
        return -1.0;
    }
    std::lock_guard<std::mutex> lock(cpu_baseline_mutex());
    CpuTicks& baseline = cpu_baseline();
    const CpuTicks previous = baseline;
    baseline = current;
    if (!previous.valid) {
        return -1.0;  // 首次调用：只建立基线
    }
    if (current.total <= previous.total) {
        return -1.0;  // 计数器无进展/回绕
    }
    const std::uint64_t total_delta = current.total - previous.total;
    const std::uint64_t idle_delta =
        current.idle > previous.idle ? current.idle - previous.idle : 0;
    if (idle_delta >= total_delta) {
        return 0.0;
    }
    const double busy = static_cast<double>(total_delta - idle_delta);
    double percent = busy * 100.0 / static_cast<double>(total_delta);
    if (percent < 0.0) {
        percent = 0.0;
    }
    if (percent > 100.0) {
        percent = 100.0;
    }
    return percent;
}

}  // namespace libmini
