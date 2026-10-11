// libmini 的进程级初始化，单独成一个翻译单元。
//
// 为什么单独成文件：这段初始化靠目标文件的静态初始化（.init_array / .ctors）
// 生效，前提是该目标文件进入最终镜像。若把它放进 libmini.cpp，那个文件里其余
// 符号只有示例 API（hello / getHelloMessage / logMessage），下游完全可以不引用，
// 于是整个目标文件不被拉入、初始化静默失效。单独成文件后，再给它一个「锚点」
// 符号，由 utils/init_anchor.h 让每个消费者隐式引用（见该头文件说明），
// 从而稳定地把它拉进链接。

#include "utils/export.h"
#include "utils/init_anchor.h"

// 必须先无条件包含 <csignal>，再判断 SIGPIPE。
//
// 这是个曾把 CI 全平台坑掉的顺序陷阱：SIGPIPE 是 <signal.h> / <csignal> 定义的
// 宏，本文件此前的写法是「先 #ifdef SIGPIPE、命中才 #include <csignal>」——
// 于是判断发生在宏尚不可见的时刻，除非有别的头文件恰好间接带进 signal.h，
// 否则条件恒为假、整段初始化被静默编译掉（Windows 本就无 SIGPIPE，症状被
// 掩盖；Linux / macOS 上表现为锚点目标文件进了链接、却没有构造函数，
// LibminiInitTest.SigpipeIsIgnoredOnPosix 读到 SIG_DFL 而失败）。
// 先包含、后判断，宏才有确定的可见性。
#include <csignal>

#ifdef SIGPIPE  // POSIX（macOS / Linux）；Windows / MinGW 无 SIGPIPE，整段编译掉

// 用 __attribute__((constructor)) 而非「带构造函数的静态对象」：
//   1) 语义更直白——这就是「库加载时跑一次」的钩子，不依赖读者理解
//      _GLOBAL__sub_I_* 是怎么把匿名命名空间静态对象变成 .init_array 条目的；
//   2) 更稳——静态对象形式在某些优化等级/链接器配置下有被整体消除的边缘情况，
//      显式 constructor 属性则由工具链直接登记进 .init_array / .ctors。
// 非 GNU 系编译器（MSVC 平台本就无 SIGPIPE、整段不编译，仅为完备性）回落到
// 静态对象形式，避免未知属性告警或报错。
#if defined(__GNUC__) || defined(__clang__)
// extern "C"：让符号名不被 C++ 名字修饰，CI 的目标文件断言才能按字面
// （nm | grep libmini_ignore_sigpipe）稳定命中，不依赖 _ZL... 修饰规则。
extern "C" {
__attribute__((constructor)) static void libmini_ignore_sigpipe(void) {
    ::signal(SIGPIPE, SIG_IGN);
}
}
#else
namespace {
struct ignore_sigpipe {
    ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }
};
ignore_sigpipe g_ignore_sigpipe;
}  // namespace
#endif

#endif

// 锚点定义。空实现；存在意义只是「被引用」，以便把本目标文件（连同上面的
// 静态初始化）拉进链接。见 utils/init_anchor.h。
extern "C" LIBMINI_API void libmini_init_anchor(void) {}
