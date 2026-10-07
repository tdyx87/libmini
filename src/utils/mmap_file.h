#ifndef LIBMINI_MMAP_FILE_H
#define LIBMINI_MMAP_FILE_H

#include <cstddef>
#include <string>

#include "export.h"

namespace libmini {

// 内存映射文件（Windows CreateFileMapping/MapViewOfFile、POSIX mmap）。
//
// 适用：大文件随机访问、读-改-写场景——省掉 read_file 的整份拷贝，
// 顺序扫描与随机 seek 都直接落在视图指针上。小文件直接用 read_file 即可。
//
//   MappedFile mf;
//   if (mf.open("big.bin")) {
//       const char* p = mf.data();          // 视图起点
//       use(p, mf.size());
//   }
//   MappedFile rw;
//   rw.open("journal.log", MapMode::ReadWrite);
//   rw.data()[0] = 'x';                     // 直接改页缓存
//   rw.flush();                             // 需要崩溃持久性时显式刷盘
//
// 语义：
//   * RAII：close()/析构自动解除映射并关闭句柄；可移动不可拷贝
//   * size 是 open 时刻的快照；外部把文件改大后视图不会跟进
//   * 空文件 open 成功：size()==0、data()（非 const）无视图返回 nullptr、
//     const 版本返回非空空串指针；映射不能改变文件大小
//   * 只读映射下写入是未定义行为（通常直接访问违例），编译期不拦
//   * 数据改完经页缓存对其他读者立即可见；flush() 才提供断电级落盘保证
enum class MapMode {
    ReadOnly,   // PROT_READ / PAGE_READONLY
    ReadWrite,  // PROT_READ|PROT_WRITE + MAP_SHARED / PAGE_READWRITE
};

class LIBMINI_API MappedFile {
public:
    MappedFile();
    ~MappedFile();

    MappedFile(MappedFile&& other);
    MappedFile& operator=(MappedFile&& other);

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // 打开并映射已存在的文件；失败（不存在/不是普通文件/权限不足/
    // 超出地址空间）返回 false 且对象回到未打开状态。已打开时先 close
    bool open(const std::string& path,
              MapMode mode = MapMode::ReadOnly);

    // 解除映射并关闭句柄（幂等，析构自动调用）。注意：不隐式 flush，
    // 断电持久性请先调 flush()
    void close();

    bool is_open() const { return open_; }
    std::size_t size() const { return size_; }

    // 视图起点。const 版本：未打开返回 nullptr，空文件返回非空空串；
    // 非 const 版本：无视图（未打开或空文件）返回 nullptr
    const char* data() const;
    char* data();

    // 把写入内容刷到磁盘（Windows FlushViewOfFile+FlushFileBuffers、
    // POSIX msync(MS_SYNC)）。未打开 / 只读 / 空文件返回 false
    bool flush();

private:
    MapMode mode_;
    std::size_t size_;
    char* view_;
    bool open_;
    int fd_;             // POSIX 文件描述符（Windows 恒为 -1）
    void* file_handle_;  // Windows 文件 HANDLE（POSIX 恒为 nullptr）
    void* map_handle_;   // Windows 映射 HANDLE（POSIX 恒为 nullptr）
};

}  // namespace libmini

#endif  // LIBMINI_MMAP_FILE_H
