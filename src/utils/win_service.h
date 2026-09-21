#ifndef LIBMINI_SERVICE_H
#define LIBMINI_SERVICE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "libmini.h"
#ifndef LIBMINI_STATIC
#ifdef LIBMINI_EXPORTS
#define LIBMINI_API __declspec(dllexport)
#else
#define LIBMINI_API __declspec(dllimport)
#endif
#else
#define LIBMINI_API
#endif

namespace libmini {

// Windows 服务状态（非 Windows 平台查询一律返回 Unknown）
enum class ServiceState {
    Unknown,
    NotFound,     // 服务未安装
    Stopped,
    StartPending,
    Running,
    StopPending,
    Paused,
};

// 服务启动类型
enum class ServiceStartType {
    AutoStart,    // 开机自启（默认）
    DemandStart,  // 手动启动
    Disabled,     // 禁用
};

// 安装描述（ServiceControl::install 用）
struct ServiceInstallDesc
{
    std::string name;         // 服务名（内部标识，如 "libmini_demo"）
    std::string display_name; // 显示名（services.msc 里看到的）
    std::string description;  // 描述（可选）
    std::string bin_path;     // 可执行文件完整路径（含参数则用引号包裹）
    ServiceStartType start_type = ServiceStartType::AutoStart;
    std::string account;      // 登录账户；空 = LocalSystem
    std::string password;     // 账户密码（域账户需要）
    std::string dependencies; // 依赖服务名，逗号分隔（可选）
};

// 服务状态快照
struct ServiceStatusInfo
{
    ServiceState state = ServiceState::Unknown;
    std::uint32_t process_id = 0;
    std::uint32_t exit_code = 0;      // Win32 退出码
    std::uint32_t wait_hint_ms = 0;   // 服务建议的等待上限
    bool accepted_stop = false;       // 是否接受 STOP 控制
    bool accepted_pause = false;
};

// ------------------ 服务控制（管理员权限）------------------
// install/uninstall/start/stop 需要管理员权限（UAC 提权后运行）；
// query/query_config 任何用户可用。
class LIBMINI_API ServiceControl
{
public:
    // 安装服务（SCT 描述写入注册表）。服务已存在返回 false。
    static bool install(const ServiceInstallDesc& desc);

    // 卸载服务；服务不存在视为成功（幂等）。
    // 若服务在运行会先尝试停止（最多等 wait_stop_ms 毫秒）。
    static bool uninstall(const std::string& name, int wait_stop_ms = 5000);

    // 启动服务（已运行视为成功）
    static bool start(const std::string& name);

    // 停止服务并发送控制码；已停止视为成功。
    static bool stop(const std::string& name, int wait_stop_ms = 10000);

    // 查询运行状态；服务不存在返回 state == ServiceState::NotFound
    static ServiceStatusInfo query(const std::string& name);

    // 查询启动类型；服务不存在返回 false
    static bool query_start_type(const std::string& name,
                                 ServiceStartType& out);

    // 修改启动类型
    static bool set_start_type(const std::string& name,
                               ServiceStartType type);

    // 等待服务到达目标状态
    static bool wait_for_state(const std::string& name, ServiceState target,
                               int timeout_ms);
};

// ------------------ 服务运行框架 ------------------
// 服务主逻辑回调；stop_event 置位后应尽快返回（SCM 会在超时后强杀）。
// 返回值成为服务的 Win32 退出码。
using ServiceMainFn = std::function<int(const std::atomic<bool>& stop_event)>;

// 暂停/继续回调（可选）：SCM 收到 PAUSE/CONTINUE 控制码时调用。
// 业务在 pause 里停止受理新请求、在 resume 里恢复。
using ServicePauseFn = std::function<void()>;

// 把"普通程序"封装为 Windows 服务的一体化入口。
//
// 典型用法（main 函数顶部调用）：
//
//   int main(int argc, char* argv[]) {
//       libmini::ServiceApp app("libmini_demo", "Libmini Demo Service");
//       app.set_run_callback([](const std::atomic<bool>& stop) {
//           while (!stop.load()) { do_work(); sleep_for_ms(500); }
//           return 0;
//       });
//       return app.run(argc, argv);   // 内部处理安装/卸载/调试/服务四种模式
//   }
//
// 命令行（须以管理员运行）：
//   <exe> install            安装服务（自动指向本 exe）
//   <exe> uninstall          卸载服务
//   <exe> start / stop       启动/停止服务
//   <exe> run                服务模式（由 SCM 调起，用户一般不手动执行）
//   <exe> console（或不带参数）调试模式：直接在前台运行回调，Ctrl+C 触发 stop
//
// 非本工具认识的参数原样返回 false（不消费），方便程序继续处理自己的参数。
class LIBMINI_API ServiceApp
{
public:
    ServiceApp(std::string name, std::string display_name);
    ~ServiceApp();

    ServiceApp(const ServiceApp&) = delete;
    ServiceApp& operator=(const ServiceApp&) = delete;

    // 服务主逻辑（须在 run() 之前设置）
    void set_run_callback(ServiceMainFn callback);

    // 服务描述（install 时写入；可选）
    void set_description(const std::string& description);

    // 安装时的启动类型（默认 AutoStart）
    void set_start_type(ServiceStartType type);

    // 日志回调：框架内部事件（启动/停止/控制码）通过它暴露给宿主程序。
    // 不设置则默认输出到 stderr。
    void set_log_callback(std::function<void(const char*)> on_log);

    // 暂停/继续回调（可选）。设置后服务向 SCM 声明接受 PAUSE/CONTINUE，
    // services.msc 与 sc pause/continue 即可用。不设置则不支持暂停。
    void set_pause_callbacks(ServicePauseFn on_pause, ServicePauseFn on_resume);

    // 处理命令行并进入对应模式，返回进程退出码：
    //   - 消费了服务相关参数（install/uninstall/start/stop/run/console）
    //     并执行完毕 → 返回 0 或 1
    //   - 参数不匹配（第一个参数不是服务命令）→ 返回 -1，主程序自行继续
    //   - 被注册为服务运行（SCM 调起，STARTUP 会带服务名参数）时
    //     自动进入服务模式，不依赖 argv
    int run(int argc, char* argv[]);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace libmini

#endif  // LIBMINI_SERVICE_H
