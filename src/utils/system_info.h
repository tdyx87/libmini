#ifndef LIBMINI_SYSTEM_INFO_H
#define LIBMINI_SYSTEM_INFO_H

#include <cstdint>
#include <string>

#include "export.h"

namespace libmini {

// 系统/进程基础信息查询（诊断日志的常用字段）。全部为快照式一次性查询。

// 主机名（取不到返回空串）
LIBMINI_API std::string hostname();

// 当前进程 PID
LIBMINI_API std::uint32_t current_pid();

// 当前进程可执行文件的绝对路径（UTF-8；取不到返回空串）
LIBMINI_API std::string executable_path();

// 逻辑处理器数量（含超线程；取不到返回 0）
LIBMINI_API int cpu_count();

// 物理内存总量 / 当前可用内存（字节；取不到返回 0）
LIBMINI_API std::uint64_t total_physical_memory();
LIBMINI_API std::uint64_t available_physical_memory();

// 路径所在卷的容量 / 剩余空间（字节；路径不存在或查询失败返回 0）
LIBMINI_API std::uint64_t disk_total_bytes(const std::string& path);
LIBMINI_API std::uint64_t disk_free_bytes(const std::string& path);

// CPU 使用率百分比（0..100，全核平均）。
// 语义是「区间占用率」：每次调用记录本次 CPU 累计值，返回距**上一次调用**之间
// 的忙碌占比——所以第一次调用只建立基线，返回 -1（没有区间可算）。
// 不支持的平台（FreeBSD）返回 -1。进程内共享一份基线，多线程安全。
// 典型用法：采集循环里每隔 1 秒调一次，把返回值当 gauge 上报。
LIBMINI_API double cpu_usage_percent();

}  // namespace libmini

#endif  // LIBMINI_SYSTEM_INFO_H
