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

#ifdef SIGPIPE  // POSIX（macOS / Linux）；Windows 无 SIGPIPE，整段编译掉
#include <csignal>
namespace {
struct ignore_sigpipe {
    ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }
};
ignore_sigpipe g_ignore_sigpipe;
}  // namespace
#endif

// 锚点定义。空实现；存在意义只是「被引用」，以便把本目标文件（连同上面的
// 静态初始化）拉进链接。见 utils/init_anchor.h。
extern "C" LIBMINI_API void libmini_init_anchor(void) {}
