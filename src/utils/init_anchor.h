#ifndef LIBMINI_UTILS_INIT_ANCHOR_H
#define LIBMINI_UTILS_INIT_ANCHOR_H

// 进程级初始化的「锚点」。
//
// 背景：libmini_init.cpp 里的初始化靠目标文件的静态初始化（.init_array /
// .ctors）生效，前提是该目标文件被链接器拉入最终镜像。静态库的归档成员只在
// 解析到未定义符号时才被拉入，而该文件除了构造初始化没有任何被引用的符号，
// 于是它常常整个不进链接，初始化静默失效（CI 的
// LibminiInitTest.SigpipeIsIgnoredOnPosix 在 Linux / macOS 全挂即由此而来）。
//
// 做法：libmini_init.cpp 导出一个锚点函数，本头文件让任何包含它的翻译单元
// 都产生一次对该锚点的**真实引用**，从而把那个目标文件（连同它的静态初始化）
// 拉进链接。锚点函数本身不做任何事，唯一目的就是「被引用」。
//
// 为什么不用 libmini.h 里的 inline 变量持有取址：那只在 ODR-use（真正取地址
// 且结果被使用）时才成立，-O2 下极易被优化掉。这里改为**函数调用**——
// 编译期无法证明它无副作用，调用点必然保留，目标文件必然被拉入。
//
// 非 POSIX 平台（无 SIGPIPE）该目标文件是空的，引用锚点不会引入任何额外代码，
// 成本为零。

#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

// 锚点：定义在 libmini_init.cpp。调用它没有可观察效果，仅为建立链接依赖。
LIBMINI_API void libmini_init_anchor(void);

#ifdef __cplusplus
}  // extern "C"

namespace libmini_detail {

// 头文件级的强制引用：构造一个（每 TU 一份的）静态对象，其构造函数调用锚点。
// 构造函数有副作用（调用外部函数），编译器不能省略；静态对象的构造又会产出
// 本 TU 的 .init_array 条目，链接时锚点未被解析，遂把 libmini_init.o 拉入。
struct init_anchor_ref {
    init_anchor_ref() { ::libmini_init_anchor(); }
};
static init_anchor_ref g_init_anchor_ref;

}  // namespace libmini_detail
#endif  // __cplusplus

#endif  // LIBMINI_UTILS_INIT_ANCHOR_H
