# libmini 服务配置实践

> 适用对象：基于 ConfigFacade 的服务端进程配置（RpcServer/RpcClient 已内置
> `apply_config` 接入）。内容：键命名约定、三层覆盖矩阵、部署样例与排错方法。
> 相关文档：[rpc_transport_guide.md](rpc_transport_guide.md)、
> [rpc_concurrency_guide.md](rpc_concurrency_guide.md)。

## TL;DR

- **键命名**：`<模块>.<参数>` 两段式（如 `rpc.max_in_flight`），环境变量自动
  映射 `<PREFIX>_<模块>_<参数>`（如 `MYAPP_RPC_MAX_IN_FLIGHT`）。
- **分层**：环境变量 > 配置文件 > 代码默认值。改部署行为只动环境变量，
  改基线行为改配置文件，代码默认值只兜底。
- **接入**：模块只认 `apply_config(facade, prefix)`；门面负责分层合并，
  模块负责键解释——两边各自演进互不耦合。
- **排错第一入口**：`source_of(key)` 直接回答「这个值是哪层给的」。

## 1. 键命名约定

### 1.1 文件键（JSON / INI）

```
<模块>.<参数>            两段式，点号分隔，全小写
```

| 键 | 含义 |
|---|---|
| `rpc.port` | RpcServer 监听端口 |
| `rpc.max_in_flight` | RpcServer 并发上限 |
| `rpc_client.pool_max` | RpcClient 连接池上限 |
| `db.path` / `log.level` | 其他模块同样两段式 |

- JSON 文件支持嵌套，自动展平：`{"rpc": {"port": 9000}}` → `rpc.port`。
- INI 天然两段式：`[rpc]` 下的 `port` → `rpc.port`。
- 文件按扩展名分流：`.json` 走 JSON，`.ini`/其他走 INI。

### 1.2 环境变量键

```
<PREFIX>_<模块>_<参数>    前缀 + 下划线分隔，大小写不敏感
```

门面归一化规则：键路径中的 `.` 与 `_` 等价、大小写不敏感——

| 文件键 | 前缀 `MYAPP_` 时的环境变量 |
|---|---|
| `rpc.port` | `MYAPP_RPC_PORT` |
| `rpc.max_in_flight` | `MYAPP_RPC_MAX_IN_FLIGHT` |
| `rpc_client.pool_max` | `MYAPP_RPC_CLIENT_POOL_MAX` |

因此**同一个键可以在文件里叫 `rpc.max_in_flight`，在环境变量里叫
`MYAPP_RPC_MAX_IN_FLIGHT`**，无需写映射代码。

### 1.3 前缀与 apply_config

`apply_config(facade, prefix)` 只消费「前缀之后」的部分：

```cpp
// 文件里有 rpc.port / rpc.max_in_flight / db.path / log.level
server.apply_config(cfg, "rpc.");        // 只吃 rpc.*，db./log. 被跳过
client.apply_config(cfg, "rpc_client."); // 客户端用独立前缀，互不干扰
```

前缀匹配同样走归一化：`"rpc."`、`"RPC_"`、`"rpc_"` 等价。

## 2. 分层覆盖矩阵

优先级：**环境变量 > 文件 > 默认值**（高层存在即屏蔽低层，取值即收口）。

| 层 | set 方式 | 典型内容 | 变更频率 |
|---|---|---|---|
| 默认值 | `set_default(key, val)` | 代码里的合理兜底 | 随代码发布 |
| 文件 | `load_file(path)` | 环境/站点基线（端口、路径） | 随部署包 |
| 环境变量 | 进程环境 | 单机/单实例差异（临时覆盖、调试、灰度） | 随时 |

```cpp
libmini::ConfigFacade cfg;
cfg.set_default("rpc.port", "8080");           // ① 代码默认
cfg.load_file("config.json");                  // ② 文件覆盖
cfg.set_env_prefix("MYAPP_");                  // ③ 环境变量再覆盖

cfg.get_int("rpc.port");                       // 一次查三层
cfg.source_of("rpc.port");                     // "env" / "file" / "default"
```

语义细节：

- 类型转换失败回落到更低层继续找；三层都失败返回给定 default。
- `bool` 口径与 IniConfig 一致：`true/yes/on`（大小写不敏感）为真。
- `apply_config` 只读 facade，不改任何层。
- 未出现的键**保持模块当前值**，未识别的键**静默跳过**——同一前缀下
  混放多个模块的配置互不干扰。

## 3. 部署样例

### 3.1 同一份配置文件管全进程

`config.json`：

```json
{
  "rpc":     { "port": 9000, "max_in_flight": 32, "worker_threads": 8,
               "overload_mode": "wait", "queue_wait_ms": 5000 },
  "rpc_client": { "pool_max": 8, "pipeline_max_in_flight": 16,
                  "timeout_ms": 3000, "retry_jitter": true },
  "log":     { "level": "info", "file": "logs/app.log" }
}
```

启动代码：

```cpp
libmini::ConfigFacade cfg;
cfg.load_file("config.json");
cfg.set_env_prefix("MYAPP_");

libmini::RpcServer server(libmini::RpcTransport::Tcp, "0.0.0.0:0");
server.apply_config(cfg, "rpc.");
server.register_method("add", ...);
server.start_background();
```

### 3.2 覆盖矩阵示例

| 场景 | 动作 | 生效值 |
|---|---|---|
| 基线 | 文件 `rpc.port: 9000` | 9000 |
| 宿主机 B 换端口 | 设 `MYAPP_RPC_PORT=9001` | 9001 |
| 临时压测提上限 | 设 `MYAPP_RPC_MAX_IN_FLIGHT=256` | 256（仅该实例） |
| 灰度单实例开调试日志 | 设 `MYAPP_LOG_LEVEL=debug` | 只影响该实例 |

### 3.3 重复键的来源透明化

测试/启动自检时打印来源：

```cpp
for (const char* k : {"rpc.port", "rpc.max_in_flight", "rpc_client.pool_max"}) {
    std::printf("%-28s = %-8s (from %s)\n", k,
                cfg.get(k).c_str(), cfg.source_of(k).c_str());
}
```

## 4. 已内置 apply_config 的模块

| 模块 | 键（前缀后） | 文档 |
|---|---|---|
| RpcServer | `port` `host` `worker_threads` `max_in_flight` `overload_mode`(`reject`/`wait`) `queue_wait_ms` `drain_timeout_ms` `retry_after_seconds` `queue_warn_threshold` `overload_message` | README「RPC 服务端」 |
| RpcClient | `timeout_ms` `max_retries` `retry_base_delay_ms` `retry_max_delay_ms` `retry_max_total_wait_ms` `retry_jitter` `pool_max` `pool_idle_ms` `pipeline_max_in_flight` | README「RPC 客户端」 |

新模块接入 checklist：

1. 模块侧提供一个 `apply_config(const ConfigFacade&, prefix)`（或逐 setter）；
2. 键全部两段式、文档列表化（同上表）；
3. 生命周期约束写进注释（如「须在 start 前设置」）；
4. 测试覆盖：全键生效、env 穿透、未识别键跳过、缺键保持现状。

## 5. 排错方法

**症状：改了配置没生效。** 按序排查：

1. **问来源**：`cfg.source_of(key)`——
   - 返回 `"default"`：文件没加载成功（路径错/JSON 语法错/INI 段名不符）
     或键名不匹配。`ConfigFacade` 不抛异常，加载失败静默空配置。
   - 返回 `"env"`：环境变量盖住了文件值——先查环境。
   - 返回 `"file"` 但值不对：确认改的是进程实际加载的那个文件
     （工作目录相关的相对路径是常见坑）。
2. **问归一化**：`MYAPP_RPC.CLIENT.PORT` 与 `MYAPP_RPC_CLIENT_PORT` 等价；
   若环境变量里有非法字符（如 `.`），归一化后仍能匹配，但建议统一用下划线。
3. **问前缀**：`apply_config` 的前缀拼错（如 `"rpc"` 少了点）时，
   `"rpc.port"` 前缀后剩 `".port"`，与模块期望的 `"port"` 不匹配 → 全部跳过。
   键不存在时模块**保持原值**而不是报错——用上面第 3.3 节的来源打印自检。
4. **问时机**：带「须在启动前设置」注释的键（worker_threads/max_in_flight/
   流水线在途上限等）在 start 之后应用不会回溯生效。
5. **问类型**：`"true"` 字符串对 `get_bool` 是真；但 `"enabled"` 这类非布尔
   词会回落默认——布尔键只用 `true/false/yes/no/on/off`。

**一条命令定位分层**（Unix 风格示意）：

```bash
MYAPP_RPC_PORT=9001 ./myserver --print-config   # 启动自检打印 source_of
```
