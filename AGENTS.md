# AGENTS.md — 给 AI 协作者的项目指南

> 面向「接手本仓库做改动的 AI 模型 / 新同事」。本文件是**入口与索引**，
> 刻意保持精简；具体细节按下面的索引跳到 `docs/agents/` 下的专题文件。
> 动手前请至少读完本文件的 §1–§2（红线）。

面向人类的完整文档在 [`README.md`](README.md)（1309 行，逐模块用法示例）；
版本历史在 [`CHANGELOG.md`](CHANGELOG.md)。

---

## 1. 这是什么

`libmini` 是一个 **C++11** 的日常工具库（不是框架、不是应用）：字符串、编码、摘要、
配置、RPC、HTTP、异步、压缩、SQLite、原生 UI 等常用能力的集合。

- **语言标准**：库本体严格 **C++11**（要兼容 MSVC 2017）。测试目录用 C++14（gtest 1.15 头文件要求）。
- **构建**：CMake（≥3.15）+ Conan 2 管依赖，CI 用 Ninja 生成器。
- **平台**：Windows（MSVC / MinGW）、Linux、macOS 三平台都要能编、能过测试。
- **产物**：静态库（默认）或共享库（`-DLIBMINI_BUILD_SHARED=ON`）。

### 目录结构

```
src/
  libmini.h              # 伞头文件：消费者只 include 这一个
  libmini.cpp            # 示例 API（hello / getHelloMessage / logMessage）
  libmini_init.cpp       # 进程级初始化（POSIX 下忽略 SIGPIPE）——见 pitfalls.md
  CMakeLists.txt         # 源清单 LIBMINI_SOURCES + 安装/导出规则
  utils/                 # 133 个头文件 + 对应 .cpp，全部模块在这里
    export.h             # LIBMINI_API 导出宏（改公开 API 必读）
    init_anchor.h        # 初始化锚点（不要随意改，见 pitfalls.md）
test/                    # gtest 套件，每个 .cpp 一个可执行文件 + add_test
apps/                    # 6 个示例应用（库的消费者，各带 --demo 自检）
tools/test_runner        # 测试聚合器（集成跑所有套件 + 浏览器仪表板）
benchmark/               # 性能基准
ci/
  gen_changelog.py       # CHANGELOG 生成/校验（提交前必跑，见 changelog.md）
  gen_grapheme_tables.py # UAX #29 字形簇属性表生成/校验
  smoke_consumer/        # find_package + link + run 的安装冒烟消费者（CI 步骤）
.github/workflows/       # ci.yml（11 个 job 门禁）/ release.yml（tag 发布）
docs/                    # 专题文档（rpc 传输/并发、配置实践）+ 本指南的专题文件
docs/agents/             # ← AI 协作者专题文档（本文件索引到那里）
```

---

## 2. 红线（违反任何一条都会让改动被拒 / CI 变红）

1. **库本体不得引入 C++14/17 特性。** 只用 C++11（`auto`、lambda、`std::thread`
   可以；`std::make_unique`、泛型 lambda、标准库 `std::optional` 不行——库自己
   实现了 `utils/optional.h`）。测试目录可用 C++14。
2. **新增公开 API 必须标 `LIBMINI_API`**（见 `utils/export.h`）。漏标 → 静态库没事，
   但**共享库不导出该符号，消费者链接直接失败**。最容易被忽略的一条。
3. **改动后必须跑通完整测试**，且**编译器告警要清零**：CI 有
   `-Wall -Wextra -Werror`（MSVC `/W4 /WX`）的 `warnings-strict` 变体。
4. **提交前必须刷新 CHANGELOG**，否则 CI 的 `changelog` job 直接失败。见
   [`changelog.md`](docs/agents/changelog.md)。
5. **新增 `.cpp` 必须加进 `src/CMakeLists.txt` 的 `LIBMINI_SOURCES`**，
   否则编译通过但链接报 `undefined reference`。
6. **别往仓库根目录堆临时文件。** 根目录已有一堆历史遗留的 `*.log` / `jobs*.json` /
   `log*.txt` / `build_*` 目录。新增的临时产物请放进 `out/` 或删除后再提交。
   `git status` 里出现你没预期的改动时，先确认是不是自己的。
7. **`.workbuddy-ai/` 是项目数据目录，不要删除或改动**（除非用户明确要求）。
8. **不要用 `--no-verify`** 跳过 hook，不要 force-push 共享历史（除非用户明确要求）。

---

## 3. 文档索引

| 我要…… | 看这里 |
|---|---|
| 新增/修改一个模块，知道完整步骤 | [`docs/agents/coding-guide.md`](docs/agents/coding-guide.md) |
| 本地构建、跑测试、提交前自查 | [`docs/agents/build-and-test.md`](docs/agents/build-and-test.md) |
| 搞清楚 CHANGELOG 门禁为什么不让我过 | [`docs/agents/changelog.md`](docs/agents/changelog.md) |
| 了解 CI 11 个 job 各查什么 | [`docs/agents/ci.md`](docs/agents/ci.md) |
| 动 `libmini_init.cpp` / 安装导出 / 平台分支前避险 | [`docs/agents/pitfalls.md`](docs/agents/pitfalls.md) |
| 特殊约定（编码/导出面/TLS/字形簇） | [`docs/agents/coding-guide.md`](docs/agents/coding-guide.md#特殊约定速查) |

---

## 4. 快速上手（TL;DR）

```bash
# 1. 构建（复用现有 build 目录）
cmake --build build --parallel

# 2. 测试
ctest --test-dir build --output-on-failure

# 3. 改代码 ……（新增 .cpp 记得加进 src/CMakeLists.txt 的 LIBMINI_SOURCES）

# 4. 严格告警自检
cmake --build build_warn --parallel      # 或新建 -DLIBMINI_WARNINGS_AS_ERRORS=ON 的目录

# 5. 刷新 CHANGELOG
python ci/gen_changelog.py
python ci/gen_changelog.py --check       # 退出码 0

# 6. 提交（英文祈使句，首词决定 CHANGELOG 分类）
git add -A && git commit -m "Add foo helper for bar"
git push origin main
```

**核心记忆点**：C++11；公开 API 标 `LIBMINI_API`；新 `.cpp` 进 `LIBMINI_SOURCES`；
测试全绿 + 告警清零；提交前刷 CHANGELOG；别动 `libmini_init.cpp` 的初始化机制
（除非你读懂了 [`pitfalls.md`](docs/agents/pitfalls.md)）；CI 11 个 job 必须全绿。

---

## 5. 参考资料

按类别汇总最常用的文件入口（相对仓库根）。各专题文件末尾另有各自主题的
更细参考清单。

### 项目文档

- [`README.md`](README.md) — 面向人类的完整文档（构建、模块清单、逐模块用法示例）
- [`CHANGELOG.md`](CHANGELOG.md) — 版本历史（Keep a Changelog 格式）
- [`docs/config_practices.md`](docs/config_practices.md) — 配置分层实践
- [`docs/rpc_transport_guide.md`](docs/rpc_transport_guide.md) — RPC 传输指南
- [`docs/rpc_concurrency_guide.md`](docs/rpc_concurrency_guide.md) — RPC 并发指南

### 构建与依赖

- [`CMakeLists.txt`](CMakeLists.txt) — 根构建脚本（标准、告警、选项、CPack）
- [`src/CMakeLists.txt`](src/CMakeLists.txt) — 源清单 `LIBMINI_SOURCES` + 安装/导出规则
- [`conanfile.py`](conanfile.py) — Conan 依赖声明
- [`build.bat`](build.bat) / [`msvc_build.cmd`](msvc_build.cmd) — 一键构建脚本
- [`src/libminiConfig.cmake.in`](src/libminiConfig.cmake.in) — `find_package` 配置模板

### 库本体

- [`src/libmini.h`](src/libmini.h) — 伞头文件（消费者唯一入口）
- [`src/utils/export.h`](src/utils/export.h) — `LIBMINI_API` 导出宏
- [`src/utils/init_anchor.h`](src/utils/init_anchor.h) + [`src/libmini_init.cpp`](src/libmini_init.cpp) — 进程级初始化（**改动前先读 [pitfalls.md](docs/agents/pitfalls.md)**）
- [`src/utils/`](src/utils/) — 全部模块（133 个头文件 + 对应实现）

### 测试

- [`test/CMakeLists.txt`](test/CMakeLists.txt) — 测试套件定义
- [`test/test_libmini.cpp`](test/test_libmini.cpp) — 基础套件（含 `LibminiInitTest`）
- [`test/data/GraphemeBreakTest.txt`](test/data/GraphemeBreakTest.txt) — UAX #29 官方用例
- [`tools/`](tools/) — `test_runner` 测试聚合器

### CI / 脚本

- [`.github/workflows/ci.yml`](.github/workflows/ci.yml) — 11 个 job 的 CI 门禁
- [`.github/workflows/release.yml`](.github/workflows/release.yml) — tag 发布
- [`ci/gen_changelog.py`](ci/gen_changelog.py) — CHANGELOG 生成/校验
- [`ci/gen_grapheme_tables.py`](ci/gen_grapheme_tables.py) — 字形簇属性表生成/校验
- [`ci/smoke_consumer/`](ci/smoke_consumer/) — `find_package` 冒烟消费者

### 外部规范

- [UAX #29 — Unicode Text Segmentation](https://www.unicode.org/reports/tr29/) — 字形簇分割规范（本库 `utils/grapheme.h` 实现依据）
- [Keep a Changelog](https://keepachangelog.com/) — CHANGELOG 格式约定
- [Semantic Versioning](https://semver.org/) — 版本号约定
