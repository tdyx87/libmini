# 构建、测试与提交前自查

> 上级索引：[`AGENTS.md`](../../AGENTS.md)。相关：[改代码指南](coding-guide.md)、
> [CI 门禁](ci.md)。

---

## Windows（本机 MinGW / Strawberry 已验证）

仓库根已有现成的 Ninja 构建目录 `build/`（Release，Strawberry MinGW）：

```bash
cmake --build build --parallel                 # 增量构建
ctest --test-dir build --output-on-failure     # 全套测试（约 3 分钟）
```

单个测试套件可直接跑二进制：

```bash
./build/bin/Release/test_libmini.exe --gtest_filter='LibminiInitTest.*'
./build/bin/Release/test_utils.exe
```

---

## 从零配置（三平台通用）

```bash
conan install . -of deps --build=missing -s build_type=Release
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=deps/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBMINI_BUILD_SWF_VIEWER=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI 用的完整开关（改 CMake 相关时请对照 [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
的矩阵）：

| 开关 | 作用 |
|---|---|
| `-DLIBMINI_BUILD_SHARED=ON` | 共享库（会额外验证导出面） |
| `-DLIBMINI_WARNINGS_AS_ERRORS=ON` | 告警升格为错误（提交前建议本地开一次） |
| `-DLIBMINI_UNICODE_FULL_GRAPHEME=ON` | 用完整 UAX #29 表 |
| `-DLIBMINI_BUILD_APPS=OFF` | 不编示例应用（加快构建） |
| `-DBUILD_TESTING=OFF` | 不编测试 |
| `-DLIBMINI_BUILD_SWF_VIEWER=OFF` | 不编 SWF 查看器（非 Windows 强制关闭） |

---

## 提交前的自查清单

- [ ] 三平台该编的都能编（本地至少编 Windows；Linux/macOS 靠 CI 验证）。
- [ ] `ctest` 全绿（本机 20/20 是基线）。
- [ ] 开 `-DLIBMINI_WARNINGS_AS_ERRORS=ON` 编一遍无告警。
- [ ] 改了公开头 → 开 `-DLIBMINI_BUILD_SHARED=ON` 编一遍（验证导出面）。
- [ ] `python ci/gen_changelog.py --check` 退出码为 0。
- [ ] `git status` 里没有意外的新增文件。

---

## 网络受限环境的提示（Windows 本机踩坑记录）

- 访问 GitHub API / `git push` 需走代理 `http://127.0.0.1:22256`，且加
  `--ssl-no-revoke`（schannel 的吊销检查会直接失败）。
- `git push` 若对 `git-receive-pack` 卡住（凭据助手在代理下挂起），
  改用内嵌凭据更稳：`https://<user>:<token>@github.com/...`；
  `--force-with-lease=<ref>:<expected-sha>` 可指定期望基线避免 stale info。
- job 日志需要 token（存在 Windows 凭据管理器，`git credential-wincred get`）。
- 沙箱可能封锁 `wsl.exe` / `reg.exe`。
- **本机 MinGW 没有 `SIGPIPE`**：POSIX 专属路径（如 `libmini_init.cpp` 的初始化）
  本地编不出来也验不了，只能靠 CI 的 Linux/macOS 变体。详见
  [pitfalls.md](pitfalls.md)。

---

## 参考

**构建入口**

- [`CMakeLists.txt`](../../CMakeLists.txt) — 标准/告警级别/构建选项/CPack 定义
- [`src/CMakeLists.txt`](../../src/CMakeLists.txt) — 库目标、依赖链接、安装/导出
- [`conanfile.py`](../../conanfile.py) — 依赖声明
- [`build.bat`](../../build.bat) — Windows 一键构建（conan install + preset）
- [`msvc_build.cmd`](../../msvc_build.cmd) — MSVC 增量构建
- [`CMakeUserPresets.json`](../../CMakeUserPresets.json) — CMake presets

**测试**

- [`test/CMakeLists.txt`](../../test/CMakeLists.txt) — 各测试套件与 `add_test` 定义
- [`tools/`](../../tools/) — `test_runner` 聚合器（终端进度 + 浏览器仪表板）
- [`apps/CMakeLists.txt`](../../apps/CMakeLists.txt) — 示例应用（各自 `--demo` 自检，参与 CTest）

**CI 参照**

- [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) — 各变体的真实开关取值
- [`ci/smoke_consumer/`](../../ci/smoke_consumer/) — 安装冒烟消费者（`find_package` + link + run）
