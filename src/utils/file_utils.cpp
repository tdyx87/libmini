#include "file_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "digest.h"
#include "encoding.h"
#include "path_utils.h"
#include "win_utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace libmini {

namespace {

// 统计信息（Windows/POSIX 各自填充）
struct PathInfo
{
    bool exists = false;
    bool is_dir = false;
    bool is_symlink = false;
    std::uint64_t size = 0;
    std::int64_t mtime_ms = 0;
};

#ifdef _WIN32
PathInfo query_info(const std::string& path)
{
    PathInfo info;
    const DWORD attrs = ::GetFileAttributesW(
        internal::utf8_to_wide(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return info;
    }
    info.exists = true;
    info.is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.is_symlink = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (::GetFileAttributesExW(internal::utf8_to_wide(path).c_str(),
                               GetFileExInfoStandard, &data)) {
        const std::uint64_t size_hi =
            static_cast<std::uint64_t>(data.nFileSizeHigh);
        info.size = (size_hi << 32) | data.nFileSizeLow;
        // FILETIME(1601-01-01 UTC) → Unix 毫秒
        const std::uint64_t ft =
            (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime)
             << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        info.mtime_ms =
            static_cast<std::int64_t>(ft / 10000ULL - 11644473600000ULL);
    }
    return info;
}
#else
PathInfo query_info(const std::string& path)
{
    PathInfo info;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        return info;
    }
    info.exists = true;
    info.is_symlink = S_ISLNK(st.st_mode);
    if (!info.is_symlink && ::stat(path.c_str(), &st) == 0) {
        info.is_dir = S_ISDIR(st.st_mode);
        info.size = static_cast<std::uint64_t>(st.st_size);
        info.mtime_ms = static_cast<std::int64_t>(st.st_mtime) * 1000;
    }
    return info;
}
#endif

std::string join_with(const std::string& dir, const std::string& name)
{
    if (dir.empty()) {
        return name;
    }
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') {
        return dir + name;
    }
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

#ifndef _WIN32  // 仅 POSIX 分支的 list_directory_detailed 使用
EntryKind classify(const PathInfo& info)
{
    if (info.is_symlink) {
        return EntryKind::Symlink;
    }
    if (info.is_dir) {
        return EntryKind::Directory;
    }
    if (info.exists) {
        return EntryKind::File;
    }
    return EntryKind::Other;
}
#endif

}  // namespace

bool file_exists(const std::string& path)
{
#ifdef _WIN32
    // W 版 API：路径按 UTF-8 解释，中文/非代码页字符无问题
    const DWORD attrs = GetFileAttributesW(internal::utf8_to_wide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

std::string read_file(const std::string& path)
{
#ifdef _WIN32
    HANDLE hFile = CreateFileW(
        internal::utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string content;
    LARGE_INTEGER size;
    if (GetFileSizeEx(hFile, &size) && size.QuadPart > 0) {
        content.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read_bytes = 0;
        if (!ReadFile(hFile, &content[0], static_cast<DWORD>(size.QuadPart),
                      &read_bytes, NULL)) {
            content.clear();
        } else {
            content.resize(read_bytes);
        }
    }
    CloseHandle(hFile);
    return content;
#else
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return content;
#endif
}

bool write_file(const std::string& path, const std::string& content)
{
#ifdef _WIN32
    HANDLE hFile = CreateFileW(
        internal::utf8_to_wide(path).c_str(), GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok =
        content.empty()
            ? TRUE
            : WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()),
                        &written, NULL);
    CloseHandle(hFile);
    return ok && (content.empty() || written == content.size());
#else
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << content;
    return file.good();
#endif
}

bool append_file(const std::string& path, const std::string& content)
{
#ifdef _WIN32
    HANDLE hFile = CreateFileW(
        internal::utf8_to_wide(path).c_str(), FILE_APPEND_DATA, 0, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok =
        content.empty()
            ? TRUE
            : WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()),
                        &written, NULL);
    CloseHandle(hFile);
    return ok && (content.empty() || written == content.size());
#else
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) return false;
    file << content;
    return file.good();
#endif
}

size_t file_size(const std::string& path)
{
#ifdef _WIN32
    HANDLE hFile = CreateFileW(
        internal::utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size)) {
        CloseHandle(hFile);
        return 0;
    }
    CloseHandle(hFile);
    return static_cast<size_t>(size.QuadPart);
#else
    struct stat stat_buf;
    if (stat(path.c_str(), &stat_buf) != 0) return 0;
    return static_cast<size_t>(stat_buf.st_size);
#endif
}

std::vector<std::string> list_directory(const std::string& path)
{
    std::vector<std::string> files;
    const std::vector<DirEntry> entries = list_directory_detailed(path);
    files.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        files.push_back(entries[i].name);
    }
    return files;
}

bool remove_file(const std::string& path)
{
#ifdef _WIN32
    return DeleteFileW(internal::utf8_to_wide(path).c_str()) != 0;
#else
    return ::unlink(path.c_str()) == 0;
#endif
}

bool remove_directory(const std::string& path)
{
#ifdef _WIN32
    return RemoveDirectoryW(internal::utf8_to_wide(path).c_str()) != 0;
#else
    return ::rmdir(path.c_str()) == 0;
#endif
}

// ------------------ 目录创建 ------------------

bool make_directory(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
#ifdef _WIN32
    if (::CreateDirectoryW(internal::utf8_to_wide(path).c_str(), NULL)) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS &&
           (GetFileAttributesW(internal::utf8_to_wide(path).c_str()) &
            FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    if (::mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    struct stat st;
    return errno == EEXIST && ::stat(path.c_str(), &st) == 0 &&
           S_ISDIR(st.st_mode);
#endif
}

bool make_directories(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    if (file_exists(path)) {
        return is_directory(path);
    }
    // 逐级补齐父目录：跳过根、盘符、UNC 前缀
    const std::string generic = path_to_generic(path);
    std::size_t pos = 0;
    if (!generic.empty() && generic[0] == '/') {
        pos = 1;
#ifdef _WIN32
        if (generic.size() >= 2 && generic[1] == '/') {  // UNC //server/share
            pos = generic.find('/', 2);
            pos = pos == std::string::npos ? generic.size()
                                           : pos + 1;  // share 之后才可创建
        }
#endif
    }
#ifdef _WIN32
    if (generic.size() >= 2 && generic[1] == ':') {
        pos = 2;
        if (generic.size() > 2 && generic[2] == '/') {
            pos = 3;
        }
    }
#endif
    while (pos < generic.size()) {
        const std::size_t next = generic.find('/', pos);
        const std::string part =
            next == std::string::npos ? generic.substr(pos)
                                      : generic.substr(pos, next - pos);
        if (next == std::string::npos) {
            pos = generic.size();
        } else {
            pos = next + 1;
        }
        if (part.empty() || part == ".") {
            continue;
        }
        const std::string prefix = generic.substr(0, next);
        if (!prefix.empty() && !file_exists(prefix)) {
            if (!make_directory(prefix)) {
                return false;
            }
        }
        (void)part;
    }
    return make_directory(path);
}

// ------------------ 复制 / 移动 / 递归删除 ------------------

bool copy_file(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    // COPY_EXISTING = 目标已存在时覆盖
    return ::CopyFileW(internal::utf8_to_wide(from).c_str(),
                       internal::utf8_to_wide(to).c_str(), FALSE) != 0;
#else
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return out.good();
#endif
}

bool copy_tree(const std::string& from, const std::string& to)
{
    const PathInfo src = query_info(from);
    if (!src.exists) {
        return false;
    }
    if (!src.is_dir) {
        return copy_file(from, to);
    }
    // 目标已存在且为目录 → 复制为 to/basename(from)
    std::string dst = to;
    if (file_exists(dst) && is_directory(dst)) {
        dst = join_with(dst, basename(path_to_generic(from)));
    }
    if (!make_directories(dst)) {
        return false;
    }
    const std::vector<DirEntry> entries = list_directory_detailed(from);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string child_from = join_with(from, entries[i].name);
        const std::string child_to = join_with(dst, entries[i].name);
        if (entries[i].kind == EntryKind::Directory) {
            if (!copy_tree(child_from, child_to)) {
                return false;
            }
        } else if (entries[i].kind == EntryKind::File) {
            if (!copy_file(child_from, child_to)) {
                return false;
            }
        }
        // 符号链接与其他类型跳过
    }
    return true;
}

bool rename_path(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    return ::MoveFileExW(internal::utf8_to_wide(from).c_str(),
                         internal::utf8_to_wide(to).c_str(),
                         MOVEFILE_COPY_ALLOWED) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool move_path(const std::string& from, const std::string& to)
{
    if (!file_exists(from)) {
        return false;
    }
    if (file_exists(to)) {
        return false;  // 目标已存在：不覆盖（与 rename_path 语义一致）
    }
    // 先尝试原子 rename（Windows 版 MOVEFILE_COPY_ALLOWED 已能跨盘符）
    if (rename_path(from, to)) {
        return true;
    }
    // 回退：复制 + 删除
    if (!copy_tree(from, to)) {
        return false;
    }
    return remove_tree(from);
}

bool remove_tree(const std::string& path)
{
    const PathInfo info = query_info(path);
    if (!info.exists) {
        return false;
    }
    if (!info.is_dir) {
        return remove_file(path);
    }
    const std::vector<DirEntry> entries = list_directory_detailed(path);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string child = join_with(path, entries[i].name);
        if (entries[i].kind == EntryKind::Directory) {
            if (!remove_tree(child)) {
                return false;
            }
        } else {
            if (!remove_file(child)) {
                return false;
            }
        }
    }
    return remove_directory(path);
}

// ------------------ 目录项与属性 ------------------

std::vector<DirEntry> list_directory_detailed(const std::string& path)
{
    std::vector<DirEntry> entries;
#ifdef _WIN32
    WIN32_FIND_DATAW findData;
    const std::wstring dirw = internal::utf8_to_wide(path);
    std::wstring pattern;
    if (dirw.empty() || dirw == L".") {
        pattern = L"*";
    } else {
        const wchar_t last = dirw[dirw.size() - 1];
        pattern = dirw + ((last == L'\\' || last == L'/') ? L"*" : L"\\*");
    }
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name(findData.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            DirEntry e;
            e.name = internal::wide_to_utf8(name);
            e.path = join_with(path, e.name);
            const DWORD attrs = findData.dwFileAttributes;
            const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_link =
                (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            e.kind = is_link ? EntryKind::Symlink
                             : (is_dir ? EntryKind::Directory : EntryKind::File);
            if (is_dir) {
                e.size = 0;
            } else {
                e.size = (static_cast<std::uint64_t>(findData.nFileSizeHigh)
                          << 32) |
                         findData.nFileSizeLow;
            }
            const std::uint64_t ft =
                (static_cast<std::uint64_t>(findData.ftLastWriteTime.dwHighDateTime)
                 << 32) |
                findData.ftLastWriteTime.dwLowDateTime;
            e.mtime_ms =
                static_cast<std::int64_t>(ft / 10000ULL - 11644473600000ULL);
            entries.push_back(std::move(e));
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(path.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            DirEntry e;
            e.name = name;
            e.path = join_with(path, name);
            const PathInfo info = query_info(e.path);
            e.kind = classify(info);
            e.size = info.is_dir ? 0 : info.size;
            e.mtime_ms = info.mtime_ms;
            entries.push_back(std::move(e));
        }
        closedir(dir);
    }
#endif
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) {
                  return a.name < b.name;
              });
    return entries;
}

bool is_directory(const std::string& path)
{
#ifdef _WIN32
    const DWORD attrs =
        GetFileAttributesW(internal::utf8_to_wide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::int64_t file_mtime_ms(const std::string& path)
{
    return query_info(path).mtime_ms;
}

// ------------------ 临时文件 ------------------

std::string temp_directory_path()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetTempPathW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) {
        std::wstring w(buf, n);
        if (!w.empty() && (w[w.size() - 1] == L'\\' || w[w.size() - 1] == L'/')) {
            w.erase(w.size() - 1);  // 去掉结尾分隔符，交给调用方拼
        }
        return internal::wide_to_utf8(w);
    }
    return std::string("C:\\Windows\\Temp");
#else
    const char* tmp = ::getenv("TMPDIR");
    if (tmp && *tmp) {
        return std::string(tmp);
    }
    return std::string("/tmp");
#endif
}

std::string unique_temp_path(const std::string& prefix, const std::string& dir)
{
    // 进程级递增计数 + 周期计数，避免同进程内名字碰撞。
    // 计时源按平台取：Windows GetTickCount；POSIX 用 CLOCK_MONOTONIC
    //（它无 Windows 依赖，必须放在平台分支之外编译）
    static unsigned long long counter = 0;
#ifdef _WIN32
    const unsigned long long tick =
        static_cast<unsigned long long>(::GetTickCount()) & 0xFFFULL;
#else
    struct ::timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned long long tick =
        static_cast<unsigned long long>(ts.tv_nsec / 1000000) & 0xFFFULL;
#endif
    const unsigned long long id = (++counter << 12) ^ tick;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(id));
    std::string base = dir.empty() ? temp_directory_path() : dir;
    return join_with(base, prefix + buf + ".tmp");
}

bool write_file_atomic(const std::string& path, const std::string& content)
{
    // 临时文件放目标同目录：保证与目标同卷，替换才是原子的
    const std::size_t sep = path.find_last_of("/\\");
    const std::string dir = (sep == std::string::npos) ? std::string(".")
                                                       : path.substr(0, sep);
    const std::string tmp = unique_temp_path(std::string(".libmini_atomic_"), dir);
#ifdef _WIN32
    const std::wstring wtmp = internal::utf8_to_wide(tmp);
    HANDLE hFile = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL ok = content.empty()
                  ? TRUE
                  : WriteFile(hFile, content.data(),
                              static_cast<DWORD>(content.size()), &written, NULL);
    if (ok && !FlushFileBuffers(hFile)) {
        ok = FALSE;  // 数据落盘失败：绝不进入替换步骤
    }
    CloseHandle(hFile);
    if (!ok || (!content.empty() && written != content.size())) {
        DeleteFileW(wtmp.c_str());
        return false;
    }
    // 原子替换已有目标；WRITE_THROUGH 让替换本身也尽量落盘
    if (!MoveFileExW(wtmp.c_str(), internal::utf8_to_wide(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wtmp.c_str());
        return false;
    }
    return true;
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    const char* p = content.data();
    std::size_t left = content.size();
    while (ok && left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
        } else if (n == 0) {
            ok = false;
        } else {
            p += n;
            left -= static_cast<std::size_t>(n);
        }
    }
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    ::close(fd);
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
#endif
}

namespace {

// 流式文件摘要的共用骨架：64KB 分块喂给更新器，内存占用恒定
std::string digest_file_hex(const std::string& path,
                            bool want_sha256)
{
#ifdef _WIN32
    FILE* f = ::_wfopen(internal::utf8_to_wide(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        return std::string();
    }
    Sha256 sha;
    Md5 md5;
    char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        if (want_sha256) {
            sha.update(buf, n);
        } else {
            md5.update(buf, n);
        }
    }
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) {
        return std::string();
    }
    return want_sha256 ? Hex::encode(sha.finish(), true)
                       : Hex::encode(md5.finish(), true);
}

}  // namespace

std::string sha256_file_hex(const std::string& path)
{
    return digest_file_hex(path, true);
}

std::string md5_file_hex(const std::string& path)
{
    return digest_file_hex(path, false);
}

}  // namespace libmini
