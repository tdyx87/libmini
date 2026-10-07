# 更新日志

本文件记录 libmini 的用户可见变更（新增 API、行为变化、缺陷修复与平台差异）。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

约定：

- **Added / 新增**：新增模块、头文件、公开 API。
- **Changed / 变更**：已有 API 的签名或语义变化（含「更严格的非法值处理」）。
- **Fixed / 修复**：缺陷修复。跨平台缺陷会标注影响平台。
- 仅影响 CI、打包脚本与测试自身的改动归入「内部」，不对使用者构成影响。

## [未发布]

本段分两部分：上面是**人工要点**（写清楚「为什么」，体量可控，发版时冻结保留），下面是**提交清单**（由
`ci/gen_changelog.py` 从 `git log` 生成，保证不漏不重）。改了提交信息就跑一次
`python ci/gen_changelog.py` 刷新清单；CI 会校验它是否与提交历史一致。

<!-- BEGIN generated:unreleased -->

> 本小节由 `ci/gen_changelog.py` 从 `git log` 生成，**勿手改**。
> 提交信息首词决定分类；判错就在正文加一行 `Category: 修复`
> （可选值：新增 / 变更 / 修复 / 内部），不想收录就加
> `Changelog-Skip: yes`。改完提交信息后跑一次
> `python ci/gen_changelog.py` 刷新。

### 提交清单

#### 新增

- Add secure_random: a real CSPRNG, and move UUID v4 onto it（[71a2c97](https://github.com/tdyx87/libmini/commit/71a2c97a0f0a82394bae3dbbfde88c40bc2e3904)）
- Add kdf: turn passwords into keys without a new dependency（[49af0b8](https://github.com/tdyx87/libmini/commit/49af0b8ebbb4294eef9486227f500d489a6bff04)）
- Add SHA-1 and SHA-512 to the digest module（[5c95416](https://github.com/tdyx87/libmini/commit/5c95416904bee1ded1d7e16aad5bdef5ecb14dda)）
- Add time-ordered v7 and namespace-derived v5 UUIDs（[9d6741b](https://github.com/tdyx87/libmini/commit/9d6741b4e6687029c6facbb6932e6eb7c773f832)）
- Add an RFC 4180 CSV reader and writer（[2734f3f](https://github.com/tdyx87/libmini/commit/2734f3f77faae68ebddea02556ca10d91a6abf29)）
- Add glob matching and recursive file lookup（[7d2a09e](https://github.com/tdyx87/libmini/commit/7d2a09e6691e9d192198204e925e83b65b2e6204)）
- Add file_utils extensions: symlink, permissions, directory_size, temp_directory（[dac8430](https://github.com/tdyx87/libmini/commit/dac843049e01f6cd33ad6e0d8048d5e400e620fe)）
- Add thread sync primitives: Semaphore, Event, OnceFlag, CancellationToken（[72353e0](https://github.com/tdyx87/libmini/commit/72353e060c8e84f1e7c2280d21b82f84dbe90151)）
- Add Base64url codec (RFC 4648 §5) with optional padding（[638c4b0](https://github.com/tdyx87/libmini/commit/638c4b0d3b9bf686091f3435f3bb26ee2401eb99)）
- Add ISO-8601 / RFC-3339 formatting and parsing to time_utils（[78dd430](https://github.com/tdyx87/libmini/commit/78dd43037f4fb369aebd93c146537c7255793cf8)）
- Add in-memory tar (ustar) reader and writer（[e0a7892](https://github.com/tdyx87/libmini/commit/e0a7892dc5c1450b1501abe0db2f0dd8aff53017)）
- Add zstd compression wrapper with decompression bomb guard（[828b5e4](https://github.com/tdyx87/libmini/commit/828b5e434cb8aae564ccf9a0114f346f8aef91f7)）
- Add BLAKE3 hash with keyed/derive modes and XOF output（[ac8d004](https://github.com/tdyx87/libmini/commit/ac8d004388ce7d7e643bb51e0060ab1a4e011335)）
- Add memory-mapped file module with RAII and dual-mode mapping（[98fa80a](https://github.com/tdyx87/libmini/commit/98fa80ac3730665ddcdedc3ea9071b0dca2c0a7d)）
- Add WebSocket client and server with RFC 6455 handshake and framing（[7c59ef0](https://github.com/tdyx87/libmini/commit/7c59ef004a3fa3a680a1cec94c60a780a4603440)）
- Add circuit breaker with closed/open/half-open state machine（[97b7acf](https://github.com/tdyx87/libmini/commit/97b7acf8598aeb8037bd16412eaaf758c83f14a6)）
- Add RPC multi-endpoint load balancing with failover（[7e1ef80](https://github.com/tdyx87/libmini/commit/7e1ef80a5859130b976bd6e8aa842ea7437359fc)）
- Add metrics registry with Prometheus text export（[57cd472](https://github.com/tdyx87/libmini/commit/57cd472bd7cea370c8d4ecf22d226f19b76d6059)）

#### 变更

- Teach the changelog tool to freeze a release and feed the release page（[c52962e](https://github.com/tdyx87/libmini/commit/c52962e0237ff9d80f7e0ee816433ad5adb1f970)）
- Use std::chrono::steady_clock in unique_temp_directory to avoid clock_gettime POSIX macro/link issues on macOS/Ubuntu（[6a1ed14](https://github.com/tdyx87/libmini/commit/6a1ed1426e9eec1b0c64ae1d96b5319e8e3fff05)）
- Skip symlink tests when create_symlink fails (sandbox may forbid symlinks)（[5dd33b0](https://github.com/tdyx87/libmini/commit/5dd33b04c9e8fb21e48f63edfc565a2969967122)）

#### 修复

- Fix three CI failures: POSIX unused param, symlink dir sizing, zstd find_dependency（[e1763c4](https://github.com/tdyx87/libmini/commit/e1763c4b24a0a2f97b002dff718f2e810cb8382e)）

#### 内部

- Rebaseline the changelog after v0.3.0 and drop dead matrix config（[bfc6d7f](https://github.com/tdyx87/libmini/commit/bfc6d7f1cdb125460017a01a546a1324e081ec6a)）
- Document thread sync primitives in README and refresh changelog（[f45fe47](https://github.com/tdyx87/libmini/commit/f45fe47cb16c72b01cf846dec1339ba40d37f28a)）

<!-- END generated:unreleased -->

## [0.3.0] - 2026-10-04

### 新增

- **hardware_info 模块**（`utils/hardware_info.h`）：机器硬件清单快照，
  与 `system_info` 的运行时指标分工。提供 CPU 型号与拓扑、网卡（MAC / IPv4 /
  IPv6）、物理磁盘（型号 / 序列号 / 总线类型）、卷（盘符 / 文件系统 / 标识）、
  主板与 BIOS 信息，以及 `primary_mac_address()` / `primary_ipv4_address()` /
  `format_mac()` 便捷接口。平台能力差异以「空字段 + 错误说明字符串」表达，
  不抛异常、不提权：Windows 走宽字符注册表与卷 API（中文型号与卷标不乱码，
  卷 GUID 反查盘符），Linux 读 `/proc` 与 `/sys`（卷标识用文件系统 UUID），
  macOS 走 `system_profiler`。
- **glob 模块**（`utils/glob.h`）：fnmatch 风格通配匹配 + 单层/递归查找。匹配是纯字符串算法（不碰文件系统，可单测、可内嵌），遍历是文件系统相关，两层分开避免语义漂移。匹配器用迭代式贪心回溯（不递归，长 pattern 不爆栈）；**边界检查必须先于取值** —— `pattern[p]` 越界读到的字节若恰是 `*`，回溯会把游标继续后推、在堆内存里一路向前扫，表现为「匹配一个长 pattern 把进程挂死」。`?` 分支必须自己推进游标，只置 `matched` 会让末尾的 continue 回到同一状态而死循环。`[a-z]` 区间的上界在 `i+2`（中间 `-` 只是连接符），读错位置会让区间退化成单字符。`glob()` 只看当前层（`sub/*.cpp` 支持字面目录前缀，多层通配归 `glob_recursive`），判定通配有无要看 **dir_part** 而非 file_part——判反会让所有带目录的 pattern 静默落回单层遍历并返回空。结果统一排序：文件系统遍历顺序不保证稳定，「同一目录两次列出顺序不同」会让调用方无法 diff。隐藏目录内部仍会被遍历，只是目录自身不出现在结果里。
- **csv 模块**（`utils/csv.h`）：RFC 4180 CSV 解析与序列化。引号只在**字段起始处**才被当作引号（字段中间的 `"` 是字面量），分隔符必须重置这个状态——否则本行第二个及之后字段的引号全部退化成字面量。行尾同时接受 LF / CRLF / 裸 CR（只认 CRLF 的解析器在 Unix 工具链产出的文件上会得到「整份文件一行」）；闭合引号后的脏数据（Excel 的 `"a" ,b`）按字面追加而不是判否。空行产出**零字段行**（与 Python csv 一致），否则调用方的列数判断会在尾随空行处突然 +1。序列化每行都带行尾（含最后一行），拼接两个 CSV 不会把末行与首行粘在一起。刻意不做类型推断（`"00123"` 必须保持字符串）也不 trim 字段内空格（RFC 规则 5）。刻意**不**校验各行字段数一致（RFC 规则 6），ragged 行原样保留——对齐是调用方的语义问题，照做会让模块在真实数据上不可用。文件不存在时 `csv_read_file` 返回 false 而非「读到 0 行」。
- **uuid 扩展**（`utils/uuid.h`）：新增 v7 时间有序 UUID（RFC 9562 §5.7）与 v5 命名空间派生 UUID（RFC 4122 §4.3，内部用上一轮补的 `Sha1`）。v7 前 48 位是 Unix 毫秒，毫秒内用进程内计数器（12 位 rand_a）递增，**单进程内严格单调递增**——这正是它相对 v4 的价值：v4 当主键时插入位置随机，写入放大与索引体积都随数据量劣化。单毫秒配额（4096 个）用尽时把内部时间戳 +1ms 而非让计数器回绕；此时内部时钟会领先真实时钟，故判断新毫秒必须用 `now > last_ms` 而非 `!=`，否则内部时钟回退会产出更小的 ID（时钟回拨同理）。另新增 `version()` / `variant()` / `timestamp_ms()` 与四个预定义命名空间。
- **digest 扩展**（`utils/digest.h`）：新增 `Sha1`（RFC 3174）与 `Sha512`（FIPS 180-4），与既有 `Md5`/`Sha256` 同构（增量 `update` + 一次性 `hex`），纯 C++ 实现、三个平台逐字节一致，不引入平台相关的系统加密库差异。SHA-1 的碰撞已被攻破，头文件里写明它只用于兼容场景。SHA-512 的长度字段是 128 位，实现上显式维护高低 64 位并在低位回绕时补高位——直接 `+=` 会在超过 2^61 字节后静默算出错误摘要。
- **kdf 模块**（`utils/kdf.h`）：PBKDF2-HMAC-SHA256 密钥派生（RFC 2898，零新增依赖，建在既有 `HmacSha256` 之上）+ 版本化口令密封。密封格式自带 magic / 版本号 / 算法 ID，可平滑迁移到 Argon2id；**头部（迭代次数、盐、nonce）整体进 GCM 的 AAD**，否则攻击者把迭代次数改成 1 就能让密钥退化成一次哈希。迭代次数在文件头里属于攻击者可控输入，故 `open` 对超出 `[kMinIterations, kMaxIterations]` 的值直接判否，而不是照着算——否则一次 `open` 就是 CPU DoS 开关。明文前置域分隔标记以区分「解密失败返回空」与「明文本来就空」。顺带给 `Aes256Gcm` 的三个 `static constexpr` 常量补了 C++11 外部定义：它们原先在消费者写 `EXPECT_EQ(x.size(), Aes256Gcm::kNonceSize)` 这类绑定到 const 引用的用法下会链接失败。
- **secure_random 模块**（`utils/secure_random.h`）：系统 CSPRNG 封装（Windows `BCryptGenRandom`、Linux `getrandom` 并退回 `/dev/urandom`、macOS `arc4random_buf`），提供随机字节 / 十六进制串 / Base64url 令牌；按字母表取样用**拒绝采样**消除取模偏置（直接 `%` 会让靠前字符多出最多 1/256 的权重，用作口令是可测的弱点）。熵源不可用时返回失败而非退化。
  `Uuid::generate()` 随之改用它：此前用 `mt19937_64` + `random_device` 播种，而 Mersenne Twister 可预测（624 个 32 位输出即还原状态）、且 MSVC/MinGW 的 `std::random_device` 本身也不是密码学实现；熵源不可用时返回 nil UUID 并可由 `is_nil()` 发现。`random_utils` 保持原样并已在头文件注明只适用于「需要一点随机性」的场景。
- **machine_fingerprint 模块**（`utils/machine_fingerprint.h`）：从 hardware_info
  的硬件清单派生「这台机器是谁」的稳定标识，用于许可证单机绑定、席位去重上报、
  设备聚合遥测。主板/整机序列号 + CPU 型号 + 物理网卡 MAC（+ 物理盘序列号）
  经归一化后 SHA-256 成 64 位十六进制 id。`FingerprintPolicy` 三档控制易变信号是否
  参与（`kStable` 换盘换网卡不变 / `kBalanced` 默认 / `kStrict` 熵最高），`confidence`
  标可信度，`signals` 暴露归一化中间层供调用方跨版本迁移，`missing` 说明哪些信号
  取不到及原因。三条稳定性设计：占位序列号过滤（`"To Be Filled By O.E.M."` 这类
  OEM 模板值会让同厂所有未填机器算出同一个指纹，不过滤许可证绑定直接失效）、虚拟
  /容器/隧道网卡 MAC 与虚拟盘排除、检出虚拟化环境时 confidence 强制压到 ≤20。
  纯函数 `canonical_fingerprint_token()` / `looks_like_virtual_machine()` /
  `looks_like_virtual_adapter()` 可直接单测与复用。
- **RPC 配置自检**：`RpcClient::config()` / `RpcServer::config()` 导出配置快照，
  新增 `RpcClientConfig` / `RpcServerConfig` 两个 `LIBMINI_API` 结构与
  `validate()` 合法性自检（沿用 `TcpConfig::validate()` 的「逐条 warn +
  整体判否」风格）。`apply_config()` 结束自动自检一次，让非法配置在第一次
  请求之前就暴露。

### 变更

- RPC 客户端与服务端的所有 setter 对负的超时 / 等待预算统一**钳到 0 并记 warn**。
  此前 `apply_config()` 的 `v >= 0` 守卫会**静默丢弃**负值，与直接调 setter 的
  行为不一致；`set_retry_max_delay_ms()` 低于退避基数时抬到基数。

### 修复

- **RPC `max_retries` 传负值时请求根本不发**：重试循环写作
  `attempt <= max_retries`，负值导致循环体一次都不进，`call()` 直接返回空串 +
  `UNKNOWN`，表现得像服务器不可达。现被钳到 0，即「至少尝试一次」。

### 内部

- 新增公开测试钩子 `RpcServer::set_test_bind_delay_ms()`（默认 0，上限 5000ms），
  用来确定性复现 0.2.1 记录的 HTTP 停机竞态窗口。
- 启动 / 停机不变量成文写入 `rpc.h`，并按传输类型分别加了回归断言。
- **CHANGELOG 清单自动化**：`ci/gen_changelog.py` 从 `git log` 派生
  未发布段的提交清单（只改 HTML 标记之间的内容），
  分类由提交信息首词决定，可用 `Category:` / `Changelog-Skip:`
  trailer 覆盖或跳过。CI 新增 `changelog` job（先跑生成器自测、再跑
  `--check`），清单与提交历史不一致直接失败。流程见 README
  「维护 CHANGELOG」。

## [0.2.1] - 2026-10-03

### 修复

- **HTTP 传输 `RpcServer::stop()` 挂死**（Windows）：cpp-httplib 0.28 的
  `Server::stop()` 以 `is_running_` 为闸门，而该标志要到 `listen_after_bind()`
  进入 accept 循环才置位；我们的 `bind_ok` 在 bind 完成即发布。停机若落在这个
  窗口内，`stop()` 会静默变成空操作，accept 循环随后进入并永久阻塞，
  `RpcServer::stop()` 里的 worker `join()` 无界等待。这是 CI 上 Windows
  `rpc_test` 套件偶发 1200s 超时的系统性根因（每次挂的用例不同，都是 fixture
  建立后立刻停机）。现在停机前按 `bind_done && bind_ok` 判断，有界轮询（5s 兜底）
  等 `is_running_` 置位后再调 `stop()`；从未 bind 或 bind 失败的服务器跳过等待。
- **跨单元断言求值顺序竞态**：`StopwatchTest` 的断言在求值顺序不确定时可能先读
  被测值再触发 `restart()`，读到污染数据。

### 内部

- 新增形状四确定性回归用例 `StopInsideBindAcceptWindowIsDeterministic`，并保留
  停机压力测试。

## [0.2.0] - 2026-10-02

### 新增

- **msgpack 模块**（`utils/msgpack.h`）：复用 nlohmann 内置编解码（零新增依赖、
  规范全兼容），提供 `JsonValue` 树与类型化封装，类型支持与 JSON 同一套。
- **proto_buf 模块**（`utils/proto_buf.h`）：手写 proto3 wire format 编解码，
  不引入 libprotobuf / protoc 但与官方实现字节级兼容；字段号键的 `JsonValue`
  树，packed repeated 字段用 `unfold_packed` 展开（wire format 无法自描述）。
  两者沿用 JSON / XML 后端的 `serialize_to_X` / `deserialize_from_X_or` 约定。
- **TcpConfig 配置校验**：新增 `validate()`，并在 `tcp.h` 写明心跳语义。

### 变更

- TCP 客户端 PING 间隔不再被 poll 粒度拉长（心跳节拍失真）。
- TCP poll 模式会话循环的等待上限改为自适应。
- `RpcServer::is_running()` 与 `wait_until_ready()` 语义对齐（HTTP 传输）。
- `TcpConfig` 加上 `LIBMINI_API` 导出（新增成员函数后必须导出，否则 DLL 消费者
  链接失败）。

### 修复

- **重试抖动围栏错误**：等待时间可能正好等于退避上限，改为严格小于上限。

## [0.1.2] - 2026-09-29

### 新增

- 发布流水线校验：artifact 逐包生成 `.sha256`，并额外产出 `SHA256SUMS` 总清单，
  publish 阶段验证传输完整性，下载后 `sha256sum -c SHA256SUMS` 一键核对。

### 修复

- 重试抖动围栏错误（与 0.2.0 同批修复，此处为其首次进入发布分支的位置）。
- `Stopwatch` restart 断言在慢速 CI 机器上不稳健。
- 连接池排队用例的等待预算放宽，以适应慢速 macOS 门禁机器。

## [0.1.1] - 2026-09-28

### 修复

- **两个仅 POSIX 触发的死锁**：曾导致整轮测试套件超时。
- **UDS 连接线程生命周期**与 exports 中的 Threads 依赖。
- **异步 executor 退休竞态**：表现为 `ubuntu-shared` job 上的回调停滞。
- **DirWatcher 的重命名 / 删除竞态**，以及 POSIX 基线快照竞态与重命名事件契约。
- `RunAfterExecutesOnce` 中一个「尚未触发」的时序断言。
- CI 健壮性：日志行缓冲、Windows 创建事件等待、调度器轮询（消除 CI 上的间歇失败）。

## [0.1.0] - 2026-09-28

首个发布版本。Windows（MSVC 2017 / x86）为主，Linux 与 macOS 为 CI 验证平台。
C++11 + CMake + Conan，静态 / 动态库均支持。

### 新增

- 基础设施：`string_utils` / `string_algo` / `lexical_cast`、`time_utils` /
  `stopwatch`、`file_utils`（含 `write_file_atomic` 原子写与流式摘要）/
  `path_utils`、`thread_utils`（线程池、`BlockingQueue`、`CountdownLatch`）、
  `json_utils` / `xml_utils` / `serialization`。
- 基础能力：`uuid`、`crc`、`encoding`（Base64 / Hex / URL）、`scope_guard`、
  `optional`、`random_utils`、`env`、`digest`（MD5 / SHA-256）、`ini_config`、
  `gzip`、`async`（延时调度器 + 令牌桶限流）、`args`、`file_lock`、`dir_watcher`、
  `win_service`、`hmac`、`process`、`lru_cache`、`base32`、`console`。
- 加密与存储：`aes_gcm`（Windows CNG / OpenSSL EVP）、`zip`（zlib，UTF-8 文件名）、
  `sqlite`。
- 网络与 RPC：`tcp`（帧协议 + 心跳保活，零第三方依赖）、`retry`、`rpc`
  （JSON RPC，HTTP / Windows 命名管道 / POSIX UDS / 裸 TCP 帧四种传输一套 API；
  可配置异步 executor、每调用超时、连接池、重试与抖动、过载保护、延迟分位、
  队列监控、spdlog 日志）、`object_pool`。
- 服务与工具模块：`http_client`、`http_server`（路径参数 / query 解析 / 前置
  过滤器 / fallback / 访问日志钩子 / 请求体上限）、`system_info`、
  `log_facade`、`config_facade`（默认值 → 文件 → 环境变量三层合并）、
  `net_addr`、`timer_wheel`。
- 文档：`docs/config_practices.md`、`docs/rpc_concurrency_guide.md`、
  `docs/rpc_transport_guide.md`。
- 基准：`benchmark/` 下 future-vs-callback 等对比用例。
- CI：`.github/workflows/ci.yml` 三平台 Release 基线 + Debug + Shared +
  warnings-strict（`-Wall -Wextra -Werror`）矩阵，全走 conan + Ninja，CTest 全套，
  并做安装后 `find_package` 冒烟（`ci/smoke_consumer`）。
- 发布：`.github/workflows/release.yml` tag 触发，三平台 CPack 产物（ZIP / TGZ）。

### 修复

- 首次 CI 打通三平台：win_service 与 RPC UDS 辅助函数的 POSIX 编译错误、
  POSIX 帧辅助与 C++11 lambda 捕获、控制台 POSIX 分支、条件 OpenSSL 依赖、
  冒烟工具链路径。
- **管道连接竞态**：监听实例切换间隙客户端会命中 `ERROR_FILE_NOT_FOUND`。
- 导出面：停用 `WINDOWS_EXPORT_ALL_SYMBOLS`，DLL 只导出 `LIBMINI_API` 标注的
  符号（新增公开 API 必须标注）。
- sqlite 的 `build_executable=False` 选项与平台相关的 64 位问题。

[未发布]: https://github.com/tdyx87/libmini/compare/v0.2.1...HEAD
[0.3.0]: https://github.com/tdyx87/libmini/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/tdyx87/libmini/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/tdyx87/libmini/compare/v0.1.2...v0.2.0
[0.1.2]: https://github.com/tdyx87/libmini/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/tdyx87/libmini/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/tdyx87/libmini/releases/tag/v0.1.0