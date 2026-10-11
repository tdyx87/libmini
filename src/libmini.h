#ifndef LIBMINI_H
#define LIBMINI_H

#include <string>

// DLL export macro（定义见 utils/export.h，此处引用单一来源）
#include "utils/export.h"

// 统一错误类型（Status/Result）放在最前：它是叶子头 + 全内联，模块头可以在
// 自己的正文里直接用 Result；见 result.h 顶部关于包含时序的说明
#include "utils/result.h"

// Include utility modules
#include "utils/string_utils.h"
#include "utils/string_algo.h"
#include "utils/lexical_cast.h"
#include "utils/time_utils.h"
#include "utils/stopwatch.h"
#include "utils/file_utils.h"
#include "utils/path_utils.h"
#include "utils/thread_utils.h"
#include "utils/json_utils.h"
#include "utils/xml_utils.h"
#include "utils/serialization.h"
#include "utils/msgpack.h"
#include "utils/proto_buf.h"
#include "utils/rpc.h"
#include "utils/uuid.h"
#include "utils/crc.h"
#include "utils/encoding.h"
#include "utils/scope_guard.h"
#include "utils/optional.h"
#include "utils/random_utils.h"
#include "utils/secure_random.h"
#include "utils/kdf.h"
#include "utils/env.h"
#include "utils/digest.h"
#include "utils/blake3.h"
#include "utils/ini_config.h"
#include "utils/gzip.h"
#include "utils/zstd.h"
#include "utils/async.h"
#include "utils/args.h"
#include "utils/csv.h"
#include "utils/glob.h"
#include "utils/file_lock.h"
#include "utils/metrics.h"
#include "utils/mmap_file.h"
#include "utils/dir_watcher.h"
#include "utils/win_service.h"
#include "utils/tcp.h"
#include "utils/trace.h"
#include "utils/websocket.h"
#include "utils/retry.h"
#include "utils/circuit_breaker.h"
#include "utils/zip.h"
#include "utils/tar.h"
#include "utils/object_pool.h"
#include "utils/aes_gcm.h"
#include "utils/console.h"
#include "utils/sqlite.h"
#include "utils/system_info.h"
#include "utils/hardware_info.h"
#include "utils/machine_fingerprint.h"
#include "utils/http_client.h"
#include "utils/http_server.h"
#include "utils/log_facade.h"
#include "utils/config_facade.h"
#include "utils/net_addr.h"
#include "utils/timer_wheel.h"

// 零依赖原生 UI：Win32/GDI 真桌面窗口（立即模式）+ 常驻子进程托管 + 两种成品
// 窗口（HTTP 服务面板 / 子进程运行器）。非 Windows 平台窗口后端缺失，但头与
// 布局代码仍在，见 native_ui.h 的说明
#include "utils/native_ui.h"
#include "utils/child_process.h"
#include "utils/ui_panels.h"
// 注意：hmac.h 依赖 digest.h 的完整类型定义，而 digest.h 与 libmini.h 存在
// 互 include（既有模式），把它放进本伞头文件会因 include guard 循环而拿不到
// Sha256/Md5 定义，故不在此引入。使用时请直接 #include "utils/hmac.h"

// 示例接口
class LIBMINI_API ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(const std::string& message) = 0;
};

// 示例函数声明
LIBMINI_API void hello();
LIBMINI_API std::string getHelloMessage();
LIBMINI_API void logMessage(ILogger* logger, const std::string& message);

#endif  // LIBMINI_H