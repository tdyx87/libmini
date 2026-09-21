#ifndef LIBMINI_H
#define LIBMINI_H

#include <string>

// DLL export macro
#ifndef LIBMINI_STATIC
#ifdef LIBMINI_EXPORTS
#define LIBMINI_API __declspec(dllexport)
#else
#define LIBMINI_API __declspec(dllimport)
#endif
#else
#define LIBMINI_API
#endif

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
#include "utils/rpc.h"
#include "utils/uuid.h"
#include "utils/crc.h"
#include "utils/encoding.h"
#include "utils/scope_guard.h"
#include "utils/optional.h"
#include "utils/random_utils.h"
#include "utils/env.h"
#include "utils/digest.h"
#include "utils/ini_config.h"
#include "utils/gzip.h"
#include "utils/async.h"
#include "utils/args.h"
#include "utils/file_lock.h"
#include "utils/dir_watcher.h"
#include "utils/win_service.h"
#include "utils/tcp.h"
#include "utils/retry.h"
#include "utils/zip.h"
#include "utils/object_pool.h"
#include "utils/aes_gcm.h"
#include "utils/console.h"
#include "utils/sqlite.h"
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