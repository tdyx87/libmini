# 改代码指南

> 上级索引：[`AGENTS.md`](../../AGENTS.md)。相关：[构建与测试](build-and-test.md)、
> [CHANGELOG 门禁](changelog.md)、[高危区域](pitfalls.md)。

---

## 新增一个模块（最常见）

假设要加 `utils/foo.h` + `utils/foo.cpp`：

1. 写 `src/utils/foo.h`：include guard 用 `LIBMINI_FOO_H`，`#include "export.h"`，
   内容放 `namespace libmini { ... }`，每个公开函数标 `LIBMINI_API`。
   **头文件只允许依赖它自己 include 的东西**——不要假设别的头已经被包含。
2. 写 `src/utils/foo.cpp`：`#include "foo.h"`，实现放 `namespace libmini {}`。
3. **把 `utils/foo.cpp` 加进 `src/CMakeLists.txt` 的 `LIBMINI_SOURCES`**
   （漏加会编译通过但链接时 `undefined reference`）。
4. 如果这个模块要暴露给消费者，在 `src/libmini.h` 里加一行 `#include "utils/foo.h"`。
   ⚠️ **注意 include 顺序与循环包含**：`libmini.h` 里已有说明，像 `hmac.h` 依赖
   `digest.h` 的完整类型，且有互 include，这类头**不要**放进伞头文件。
5. 写测试：加到 `test/test_XXX.cpp`（或新建套件，在 `test/CMakeLists.txt` 里
   `add_executable` + `add_test`，并记得非共享构建时
   `target_compile_definitions(... LIBMINI_STATIC)`——照抄现有块的写法）。
6. 跑构建 + 测试（见 [build-and-test.md](build-and-test.md)），刷 CHANGELOG
   （见 [changelog.md](changelog.md)），提交。

### 极简骨架参考

`src/utils/foo.h`：

```cpp
#ifndef LIBMINI_FOO_H
#define LIBMINI_FOO_H

#include <string>

#include "export.h"

namespace libmini {

// 一句话说明这个函数做什么、边界条件是什么
LIBMINI_API std::string foo_bar(const std::string& input);

}  // namespace libmini

#endif  // LIBMINI_FOO_H
```

`src/utils/foo.cpp`：

```cpp
#include "foo.h"

namespace libmini {

std::string foo_bar(const std::string& input) {
    return input;
}

}  // namespace libmini
```

---

## 修改既有模块

- 改实现：动 `utils/*.cpp`；改接口：动 `utils/*.h`，并同步更新受影响的
  `test/`、`example/`、`apps/` 调用点（编译器会告诉你哪些）。
- **保持向后兼容是默认要求**。除非用户明确要求，不要改已有函数签名 / 删函数。
- 动了公开头就重新评估：是否需要 `LIBMINI_API`；是否影响共享库导出面。

---

## 提交信息风格

仓库用**英文祈使句**、一句话概括，首词决定 CHANGELOG 分类
（`Add`→新增，`Fix`→修复，`Document`/`Bump`→内部，其余按关键词表落到「变更」）。
示例（来自真实历史）：

```
Fix WebSocket sockets to suppress SIGPIPE on macOS
Init: ignore SIGPIPE at library load time on POSIX
Assert the init hook is linked in, drop the debug dump
Cover run_process concurrency and timeout paths
```

正文（可选）说明**为什么**这么改，而不是复述 diff。需要手动指定分类或跳过收录时
用 trailer，见 [changelog.md](changelog.md)。

---

## 特殊约定速查

- **UTF-8**：源码统一 UTF-8；MSVC 靠根 `CMakeLists.txt` 的 `/utf-8`。
- **Windows API 目标版本**：根 `CMakeLists.txt` 全局定义 `_WIN32_WINNT=0x0A00`，
  这是 cpp-httplib 0.28+ 的硬要求，**必须在任何 `Windows.h` 之前定义**。
- **导出面**：只有标了 `LIBMINI_API` 的符号进 DLL/SO。**刻意不用**
  `WINDOWS_EXPORT_ALL_SYMBOLS`（会把内部符号、STL 实例化、第三方符号一起导出）。
- **TLS/HTTPS 是可选的**：找不到 OpenSSL 时整体关闭，相关测试自动跳过，
  运行期 https 报明确错误。改网络模块时注意这个分支。
- **字形簇表**：默认启发式表（`grapheme_tables_heuristic.inc`）；完整表由
  `ci/gen_grapheme_tables.py` 从 UCD 生成并**随仓库提交**（编译不联网）。
  符合性测试 `GraphemeTest.Uax29OfficialTestFile` 每次构建逐条断言：
  完整表要求 853/853，启发式允许 852/853（一条已知缺口）。
  改了相关代码记得 `python ci/gen_grapheme_tables.py --check`。
- **示例应用**（`apps/`）默认参与构建与 CTest，每个都有 `--demo` 自检模式。
- **原生 UI 栈**（`native_ui` / `child_process` / `ui_panels`）只在 Windows 有真实
  实现；非 Windows 下 `native_ui.cpp` 编成空实现、不链接任何 GUI 库。

---

## 参考

**改代码会碰到的文件**

- [`src/CMakeLists.txt`](../../src/CMakeLists.txt) — 源清单 `LIBMINI_SOURCES`（新增 `.cpp` 必须登记）
- [`src/libmini.h`](../../src/libmini.h) — 伞头文件（决定哪些模块暴露给消费者）
- [`src/utils/export.h`](../../src/utils/export.h) — `LIBMINI_API` 宏定义
- [`src/utils/string_utils.h`](../../src/utils/string_utils.h) / [`string_utils.cpp`](../../src/utils/string_utils.cpp) — 头/源写法参考
- [`src/utils/utf8_codec.h`](../../src/utils/utf8_codec.h) — 内部实现细节，**不对外安装**（安装规则里有 EXCLUDE）
- [`test/CMakeLists.txt`](../../test/CMakeLists.txt) — 新增测试套件的写法参考

**提交信息与分类**

- [`ci/gen_changelog.py`](../../ci/gen_changelog.py) — 分类关键词表（首词映射到 新增/变更/修复/内部）
- [`CHANGELOG.md`](../../CHANGELOG.md) — 分类的实际效果

**字形簇相关**

- [`src/utils/grapheme_tables_heuristic.inc`](../../src/utils/grapheme_tables_heuristic.inc) — 默认启发式表
- [`src/utils/grapheme_tables_full.inc`](../../src/utils/grapheme_tables_full.inc) — 完整表（生成产物，随仓库提交）
- [`ci/gen_grapheme_tables.py`](../../ci/gen_grapheme_tables.py) — 表生成/校验脚本
- [UAX #29](https://www.unicode.org/reports/tr29/) — 外部规范
