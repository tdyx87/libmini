#include "mmap_file.h"

#include "win_utf.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace libmini {

MappedFile::MappedFile()
    : mode_(MapMode::ReadOnly),
      size_(0),
      view_(nullptr),
      open_(false),
      fd_(-1),
      file_handle_(nullptr),
      map_handle_(nullptr)
{
}

MappedFile::~MappedFile()
{
    close();
}

MappedFile::MappedFile(MappedFile&& other)
    : mode_(other.mode_),
      size_(other.size_),
      view_(other.view_),
      open_(other.open_),
      fd_(other.fd_),
      file_handle_(other.file_handle_),
      map_handle_(other.map_handle_)
{
    other.view_ = nullptr;
    other.size_ = 0;
    other.open_ = false;
    other.fd_ = -1;
    other.file_handle_ = nullptr;
    other.map_handle_ = nullptr;
}

MappedFile& MappedFile::operator=(MappedFile&& other)
{
    if (this != &other) {
        close();
        mode_ = other.mode_;
        size_ = other.size_;
        view_ = other.view_;
        open_ = other.open_;
        fd_ = other.fd_;
        file_handle_ = other.file_handle_;
        map_handle_ = other.map_handle_;
        other.view_ = nullptr;
        other.size_ = 0;
        other.open_ = false;
        other.fd_ = -1;
        other.file_handle_ = nullptr;
        other.map_handle_ = nullptr;
    }
    return *this;
}

#ifdef _WIN32

bool MappedFile::open(const std::string& path, MapMode mode)
{
    close();
    mode_ = mode;

    const DWORD access = (mode == MapMode::ReadWrite)
                             ? (GENERIC_READ | GENERIC_WRITE)
                             : GENERIC_READ;
    HANDLE file = ::CreateFileW(internal::utf8_to_wide(path).c_str(), access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    // 查询文件大小（LARGE_INTEGER 版本，避免句柄类型转换问题）
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(file, &li)) {
        ::CloseHandle(file);
        return false;
    }
    const std::size_t fsz = static_cast<std::size_t>(li.QuadPart);

    file_handle_ = file;
    size_ = fsz;
    open_ = true;

    if (fsz == 0) {
        // 空文件无法建立映射：保持打开但无视图
        return true;
    }

    const DWORD protect = (mode == MapMode::ReadWrite) ? PAGE_READWRITE
                                                       : PAGE_READONLY;
    HANDLE mapping = ::CreateFileMappingW(file, nullptr, protect, 0, 0, nullptr);
    if (mapping == nullptr) {
        close();
        return false;
    }

    const DWORD map_access =
        (mode == MapMode::ReadWrite) ? FILE_MAP_WRITE : FILE_MAP_READ;
    void* view = ::MapViewOfFile(mapping, map_access, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        close();
        return false;
    }

    map_handle_ = mapping;
    view_ = static_cast<char*>(view);
    return true;
}

void MappedFile::close()
{
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (map_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(map_handle_));
        map_handle_ = nullptr;
    }
    if (file_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
    size_ = 0;
    open_ = false;
}

bool MappedFile::flush()
{
    if (!open_ || view_ == nullptr || size_ == 0 ||
        mode_ != MapMode::ReadWrite) {
        return false;
    }
    if (!::FlushViewOfFile(view_, size_)) {
        return false;
    }
    return ::FlushFileBuffers(static_cast<HANDLE>(file_handle_)) != FALSE;
}

#else  // POSIX

bool MappedFile::open(const std::string& path, MapMode mode)
{
    close();
    mode_ = mode;

    const int flags = (mode == MapMode::ReadWrite) ? O_RDWR : O_RDONLY;
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        ::close(fd);
        return false;
    }
    const std::size_t fsz = static_cast<std::size_t>(st.st_size);

    fd_ = fd;
    size_ = fsz;
    open_ = true;

    if (fsz == 0) {
        // 空文件无法 mmap：保持打开但无视图
        return true;
    }

    const int prot = (mode == MapMode::ReadWrite) ? (PROT_READ | PROT_WRITE)
                                                  : PROT_READ;
    void* view = ::mmap(nullptr, fsz, prot, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        close();
        return false;
    }

    view_ = static_cast<char*>(view);
    return true;
}

void MappedFile::close()
{
    if (view_ != nullptr) {
        ::munmap(view_, size_);
        view_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
    open_ = false;
}

bool MappedFile::flush()
{
    if (!open_ || view_ == nullptr || size_ == 0 ||
        mode_ != MapMode::ReadWrite) {
        return false;
    }
    return ::msync(view_, size_, MS_SYNC) == 0;
}

#endif  // _WIN32

const char* MappedFile::data() const
{
    if (!open_) {
        return nullptr;
    }
    if (view_ != nullptr) {
        return view_;
    }
    // 空文件：返回非空空串指针，便于与普通缓冲区一致地做指针运算
    static const char kEmpty[] = "";
    return kEmpty;
}

char* MappedFile::data()
{
    return (open_ && view_ != nullptr) ? view_ : nullptr;
}

}  // namespace libmini
