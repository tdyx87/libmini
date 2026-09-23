#include "system_info.h"

#include "win_utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
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
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf, static_cast<std::size_t>(n));
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

}  // namespace libmini
