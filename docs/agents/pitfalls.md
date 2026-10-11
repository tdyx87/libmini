# 高危区域（改这些请格外小心）

> 上级索引：[`AGENTS.md`](../../AGENTS.md)。这几处历史上栽过跟头，改动前请读完本节。

---

## 7.1 进程级初始化：`libmini_init.cpp` / `utils/init_anchor.h` / `libmini.h`

这段代码**看起来多余，其实不能删**。它做一件事：POSIX 下在库加载时把 `SIGPIPE`
设为 `SIG_IGN`（否则第三方代码往已关闭的 socket 写会被信号杀掉进程）。

它是**跨平台行为差异 + 静态库链接语义的雷区**，历史上在这上面栽过多次。三条铁律：

### 铁律 1：必须先 `#include <csignal>` 再 `#ifdef SIGPIPE`

`SIGPIPE` 是 `<csignal>` 自己定义的宏。**判断写在包含之前 → 宏不可见 → 条件恒假
→ 整段被静默编译掉。**

```cpp
// ❌ 错误：判断时 SIGPIPE 还不可见
#ifdef SIGPIPE
#include <csignal>
namespace { struct ignore_sigpipe { ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); } }; ignore_sigpipe g; }
#endif

// ✅ 正确：先包含，后判断
#include <csignal>
#ifdef SIGPIPE
extern "C" { __attribute__((constructor)) static void libmini_ignore_sigpipe(void) {
    ::signal(SIGPIPE, SIG_IGN);
} }
#endif
```

> 这正是曾经的 **CI 全平台失败根因**：锚点在、`.init_array` 在，却从没人设过
> SIGPIPE。Windows 因天然无 SIGPIPE 而掩盖了症状。

### 铁律 2：初始化所在的目标文件必须真的被链接进来

静态库的归档成员**只在解析到未定义符号时才被拉入**。本文件除构造初始化外没有
别的被引用符号 → 默认不进链接 → 初始化静默失效。

解决办法是「**锚点符号**」（`extern "C" LIBMINI_API void libmini_init_anchor(void)`）：
`utils/init_anchor.h`（被 `libmini.h` 包含）让每个消费者隐式引用它，从而把整个
目标文件拉进链接。

**不要**改成「给下游加 `target_link_options(... INTERFACE "$<TARGET_OBJECTS:...>")`
强制拉入目标文件」——那会写进安装导出的 `libminiTargets.cmake`，令
`find_package(libmini)` 的消费者在配置期报
`Objects of target "libmini_init" referenced but no such target exists.`
（实测教训，`src/CMakeLists.txt` 上有注释）。

### 铁律 3：CI 有断言，别绕过

非 Windows 变体会 `nm` 检查 `libmini_init.cpp.o` 里必须有加载期构造函数
（`libmini_ignore_sigpipe` 或 `_GLOBAL__sub_I`），没有就**构建期失败**。

### 相关测试与验证限制

- 测试：`test/test_libmini.cpp` 的 `LibminiInitTest.SigpipeIsIgnoredOnPosix`
  （`#ifndef _WIN32` 才跑）。
- **本机 MinGW 没有 `SIGPIPE`，无法本地验证 POSIX 路径**，只能靠 CI 的
  Linux/macOS 变体。改这里一定要盯 CI 结果。

---

## 7.2 归档 / 安装 / 导出：`src/CMakeLists.txt`

`install(TARGETS ... EXPORT libminiTargets)` + `install(EXPORT ...)` 决定了
下游 `find_package(libmini)` 能不能用。

**任何 `INTERFACE` 的链接选项 / 编译定义都会泄漏进导出文件**，进而影响所有消费者。
改这里请本地跑一次 `ci/smoke_consumer`（configure + build + run）验证：

```bash
cmake -S ci/smoke_consumer -B smoke/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<abs>/deps/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH=<abs>/smoke/prefix
cmake --build smoke/build --parallel
./smoke/build/consumer
```

（`CMAKE_TOOLCHAIN_FILE` 相对路径是相对**源目录**解析的，务必用绝对路径。）

---

## 7.3 条件编译与平台分支

`_WIN32` / `__APPLE__` / `__linux__` 分支很多，且**本机能验证的只有 Windows**。
新增平台分支时，宁可用 `#ifdef` 包住整段并提供另一平台的等价实现 / 空实现
（参考 `native_ui.cpp` 在非 Windows 下编成空实现），也不要让某平台编不过。

涉及 socket 信号行为的代码（`rpc` / `tcp` / `websocket`）历史上多次因 SIGPIPE
在不同平台表现不一致出问题——改这些优先考虑「有没有别的平台专属信号/错误码」。

---

## 参考

**7.1 初始化机制（改前必读源码）**

- [`src/libmini_init.cpp`](../../src/libmini_init.cpp) — 初始化实现（含 `#include <csignal>` 顺序的注释说明）
- [`src/utils/init_anchor.h`](../../src/utils/init_anchor.h) — 锚点机制与其原理注释
- [`src/libmini.h`](../../src/libmini.h) — 引入锚点的位置
- [`src/CMakeLists.txt`](../../src/CMakeLists.txt) — 为何不用 INTERFACE 链接选项的注释（第 76 行起）
- [`test/test_libmini.cpp`](../../test/test_libmini.cpp) — `LibminiInitTest.SigpipeIsIgnoredOnPosix`
- [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) — `Verify the init constructor is compiled in` 断言步骤

**7.2 安装 / 导出**

- [`src/CMakeLists.txt`](../../src/CMakeLists.txt) — `install(TARGETS/EXPORT)` 与 `configure_package_config_file`
- [`src/libminiConfig.cmake.in`](../../src/libminiConfig.cmake.in) — 消费者侧的 config 模板
- [`ci/smoke_consumer/`](../../ci/smoke_consumer/) — 验证导出的冒烟消费者

**7.3 平台分支**

- [`src/utils/native_ui.cpp`](../../src/utils/native_ui.cpp) — 「非 Windows 编成空实现」的范例
- [`src/utils/websocket.cpp`](../../src/utils/websocket.cpp) / [`tcp.cpp`](../../src/utils/tcp.cpp) / [`rpc.cpp`](../../src/utils/rpc.cpp) — SIGPIPE 相关历史改动点
- [`src/utils/export.h`](../../src/utils/export.h) — 平台分支下的导出宏定义
