# CI 门禁一览

> 上级索引：[`AGENTS.md`](../../AGENTS.md)。工作流文件：[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)。

push / PR 到 `main` 触发，共 **11 个 job**：

| Job | 平台 | 验证内容 |
|---|---|---|
| `ubuntu-release` / `windows-release` / `macos-release` | 三平台 | Release 静态库基线：构建 + 全套 CTest + 安装 + 冒烟消费者 |
| `ubuntu-debug` / `windows-debug` | 两平台 | Debug（断言/STL 检查生效） |
| `ubuntu-shared` / `windows-shared` | 两平台 | 共享库导出面 + 消费者链接 |
| `ubuntu-warnings-strict` | Linux | `-Wall -Wextra -Werror` 零告警 |
| `ubuntu-full-grapheme` / `windows-full-grapheme` | 两平台 | `-DLIBMINI_UNICODE_FULL_GRAPHEME=ON` 完整表 + 853/853 断言 |
| `changelog` | Linux | 生成器自测 + CHANGELOG 与 git log 一致 |

**全部 job 必须绿**——`release.yml` 的发布门禁 `ci-gate` 要求如此。

## 每个变体的固定步骤

1. `actions/checkout`
2. （Windows）`ilammy/msvc-dev-cmd` 激活 MSVC 环境（Ninja 下 CMake 找不到 `cl.exe`）
3. 安装 Conan，配置 profile（注入 `compiler.cppstd=14` 等）
4. `conan install` → `cmake configure` → `cmake --build`
5. 非 Windows 变体：断言 `libmini_init.cpp.o` 含加载期构造函数（见 [pitfalls.md](pitfalls.md)）
6. `ctest`（Linux 用 `stdbuf -oL` 行缓冲，避免套件被超时杀掉时丢输出）
7. `cmake --install` 到前缀 → 跑 `ci/smoke_consumer`（`find_package` + link + run）

## 发布工作流

`release.yml` 在 tag（`v*`）时触发：先以可复用工作流重跑同提交的完整 CI，
**全绿才打包**；三平台 CPack 产物（ZIP/TGZ）逐包带 `.sha256`，
publish 阶段验证 artifact 传输完整性，Release 页附 `SHA256SUMS` 总清单。

## 与本地验证的差异

本机（Windows MinGW）只能覆盖三平台里的一个，且**没有 `SIGPIPE`**，
所以 Linux/macOS 专属行为（初始化器、socket 信号处理等）必须靠 CI 结果确认。
改了平台相关代码，务必盯 CI 的 Linux/macOS job。

---

## 参考

- [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) — 本文件描述的矩阵与步骤
- [`.github/workflows/release.yml`](../../.github/workflows/release.yml) — 发布工作流与 `ci-gate`
- [`ci/smoke_consumer/CMakeLists.txt`](../../ci/smoke_consumer/CMakeLists.txt) + [`main.cpp`](../../ci/smoke_consumer/main.cpp) — 安装冒烟消费者
- [`ci/gen_changelog.py`](../../ci/gen_changelog.py) / [`ci/gen_grapheme_tables.py`](../../ci/gen_grapheme_tables.py) — `changelog` job 跑的两个生成器
- [`ci/test_gen_changelog.py`](../../ci/test_gen_changelog.py) / [`ci/test_gen_grapheme_tables.py`](../../ci/test_gen_grapheme_tables.py) — job 里的生成器自测
- [`conanfile.py`](../../conanfile.py) — CI 安装依赖的依据
- [`src/CMakeLists.txt`](../../src/CMakeLists.txt) — 初始化构造函数断言的检查对象
