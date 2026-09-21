#include "win_service.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "path_utils.h"
#include "win_utf.h"

namespace libmini {

#ifdef _WIN32

namespace {

ServiceState from_scm_state(DWORD s)
{
    switch (s) {
        case SERVICE_STOPPED:       return ServiceState::Stopped;
        case SERVICE_START_PENDING: return ServiceState::StartPending;
        case SERVICE_RUNNING:       return ServiceState::Running;
        case SERVICE_STOP_PENDING:  return ServiceState::StopPending;
        case SERVICE_PAUSED:        return ServiceState::Paused;
        default:                    return ServiceState::Unknown;
    }
}

DWORD to_start_type(ServiceStartType t)
{
    switch (t) {
        case ServiceStartType::DemandStart: return SERVICE_DEMAND_START;
        case ServiceStartType::Disabled:    return SERVICE_DISABLED;
        default:                            return SERVICE_AUTO_START;
    }
}

}  // namespace

// ------------------ ServiceControl ------------------

namespace {

SC_HANDLE open_scm(DWORD access)
{
    return ::OpenSCManagerW(NULL, NULL, access);
}

SC_HANDLE open_service(SC_HANDLE scm, const std::string& name, DWORD access)
{
    return ::OpenServiceW(scm, internal::utf8_to_wide(name).c_str(), access);
}

}  // namespace

bool ServiceControl::install(const ServiceInstallDesc& desc)
{
    if (desc.name.empty() || desc.bin_path.empty()) {
        return false;
    }
    SC_HANDLE scm = open_scm(SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        return false;
    }
    const std::wstring wpath = internal::utf8_to_wide(desc.bin_path);
    const std::wstring wdeps =
        desc.dependencies.empty()
            ? std::wstring()
            : internal::utf8_to_wide(desc.dependencies);

    SC_HANDLE svc = ::CreateServiceW(
        scm, internal::utf8_to_wide(desc.name).c_str(),
        internal::utf8_to_wide(
            desc.display_name.empty() ? desc.name : desc.display_name)
            .c_str(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        to_start_type(desc.start_type), SERVICE_ERROR_NORMAL, wpath.c_str(),
        NULL, NULL, wdeps.empty() ? NULL : wdeps.c_str(),
        desc.account.empty()
            ? NULL
            : internal::utf8_to_wide(desc.account).c_str(),
        desc.password.empty()
            ? NULL
            : internal::utf8_to_wide(desc.password).c_str());
    if (!svc) {
        ::CloseServiceHandle(scm);
        return false;
    }

    // 描述写注册表（ChangeServiceConfig2W）
    if (!desc.description.empty()) {
        SERVICE_DESCRIPTIONW sd;
        std::wstring wdesc = internal::utf8_to_wide(desc.description);
        sd.lpDescription = const_cast<LPWSTR>(wdesc.c_str());
        ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);
    }

    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return true;
}

bool ServiceControl::uninstall(const std::string& name, int wait_stop_ms)
{
    const ServiceStatusInfo st = query(name);
    if (st.state == ServiceState::NotFound) {
        return true;  // 幂等
    }
    if (st.state != ServiceState::Stopped &&
        st.state != ServiceState::Unknown) {
        stop(name, wait_stop_ms);
    }

    SC_HANDLE scm = open_scm(SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        return false;
    }
    SC_HANDLE svc = open_service(scm, name, DELETE);
    if (!svc) {
        ::CloseServiceHandle(scm);
        return false;
    }
    const BOOL ok = ::DeleteService(svc);
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return ok != 0;
}

bool ServiceControl::start(const std::string& name)
{
    const ServiceStatusInfo st = query(name);
    if (st.state == ServiceState::NotFound) {
        return false;
    }
    if (st.state == ServiceState::Running) {
        return true;  // 幂等
    }
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) {
        return false;
    }
    SC_HANDLE svc = open_service(scm, name, SERVICE_START);
    if (!svc) {
        ::CloseServiceHandle(scm);
        return false;
    }
    const BOOL ok = ::StartServiceW(svc, 0, NULL);
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return ok != 0;
}

bool ServiceControl::stop(const std::string& name, int wait_stop_ms)
{
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) {
        return false;
    }
    SC_HANDLE svc =
        open_service(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        ::CloseServiceHandle(scm);
        return false;
    }
    SERVICE_STATUS st = {};
    const BOOL sent = ::ControlService(svc, SERVICE_CONTROL_STOP, &st);
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    if (!sent && ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        return false;
    }
    return wait_for_state(name, ServiceState::Stopped, wait_stop_ms);
}

ServiceStatusInfo ServiceControl::query(const std::string& name)
{
    ServiceStatusInfo out;
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) {
        return out;
    }
    SC_HANDLE svc = open_service(scm, name, SERVICE_QUERY_STATUS);
    if (!svc) {
        ::CloseServiceHandle(scm);
        out.state = ::GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST
                        ? ServiceState::NotFound
                        : ServiceState::Unknown;
        return out;
    }
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (::QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                               reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                               &needed)) {
        out.state = from_scm_state(ssp.dwCurrentState);
        out.process_id = ssp.dwProcessId;
        out.exit_code = ssp.dwWin32ExitCode;
        out.wait_hint_ms = ssp.dwWaitHint;
        out.accepted_stop =
            (ssp.dwControlsAccepted & SERVICE_ACCEPT_STOP) != 0;
        out.accepted_pause =
            (ssp.dwControlsAccepted & SERVICE_ACCEPT_PAUSE_CONTINUE) != 0;
    }
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return out;
}

bool ServiceControl::query_start_type(const std::string& name,
                                      ServiceStartType& out)
{
    SC_HANDLE scm = open_scm(SC_MANAGER_CONNECT);
    if (!scm) {
        return false;
    }
    SC_HANDLE svc = open_service(scm, name, SERVICE_QUERY_CONFIG);
    if (!svc) {
        ::CloseServiceHandle(scm);
        return false;
    }
    bool ok = false;
    DWORD needed = 0;
    ::QueryServiceConfigW(svc, NULL, 0, &needed);
    if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed > 0) {
        std::vector<std::uint8_t> buf(needed);
        QUERY_SERVICE_CONFIGW* cfg =
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        if (::QueryServiceConfigW(svc, cfg, needed, &needed)) {
            switch (cfg->dwStartType) {
                case SERVICE_DEMAND_START:
                    out = ServiceStartType::DemandStart;
                    break;
                case SERVICE_DISABLED:
                    out = ServiceStartType::Disabled;
                    break;
                default:
                    out = ServiceStartType::AutoStart;
                    break;
            }
            ok = true;
        }
    }
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return ok;
}

bool ServiceControl::set_start_type(const std::string& name,
                                    ServiceStartType type)
{
    SC_HANDLE scm = open_scm(SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        return false;
    }
    SC_HANDLE svc = open_service(scm, name, SERVICE_CHANGE_CONFIG);
    if (!svc) {
        ::CloseServiceHandle(scm);
        return false;
    }
    const BOOL ok = ::ChangeServiceConfigW(
        svc, SERVICE_NO_CHANGE, to_start_type(type), SERVICE_NO_CHANGE, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL);
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return ok != 0;
}

bool ServiceControl::wait_for_state(const std::string& name,
                                    ServiceState target, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const ServiceStatusInfo st = query(name);
        if (st.state == target) {
            return true;
        }
        if (st.state == ServiceState::NotFound) {
            return target == ServiceState::NotFound;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // 用 wait_hint 自适应轮询间隔（钳制在 50~500ms）
        int delay = st.wait_hint_ms > 0
                        ? static_cast<int>(st.wait_hint_ms / 10)
                        : 100;
        if (delay < 50) delay = 50;
        if (delay > 500) delay = 500;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

// ------------------ ServiceApp ------------------

struct ServiceApp::Impl
{
    std::string name;
    std::string display_name;
    std::string description;
    ServiceStartType start_type = ServiceStartType::AutoStart;
    ServiceMainFn run_callback;
    std::function<void(const char*)> log_callback;

    std::atomic<bool> stop_event{false};
    std::atomic<bool> paused{false};
    ServicePauseFn on_pause;
    ServicePauseFn on_resume;
    SERVICE_STATUS_HANDLE status_handle = NULL;
    SERVICE_STATUS status = {};
    DWORD checkpoint = 0;
    int app_exit_code = 0;

    // own-process 服务：每进程一个活动实例。
    // SCM 的 ServiceMain / 控制处理器都是裸函数指针，不带上下文，
    // 用该静态指针桥接（构造时设置、析构时清除）
    static Impl* s_active;

    explicit Impl(std::string n, std::string dn)
        : name(std::move(n)), display_name(std::move(dn))
    {
    }

    ~Impl() { s_active = nullptr; }

    void log(const char* msg)
    {
        if (log_callback) {
            log_callback(msg);
        } else {
            std::fprintf(stderr, "[service] %s\n", msg);
        }
    }

    bool report(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 3000)
    {
        if (status_handle == NULL) {
            return true;  // 调试模式：无 SCM 可报告
        }
        status.dwCurrentState = state;
        status.dwWin32ExitCode = exit_code;
        status.dwWaitHint = wait_hint;
        if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
            status.dwCheckPoint = ++checkpoint;  // pending 状态递增检查点
            status.dwControlsAccepted = 0;
        } else {
            status.dwCheckPoint = 0;
            // 配置了暂停回调才声明 PAUSE/CONTINUE 接受位
            status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                (on_pause ? (SERVICE_ACCEPT_PAUSE_CONTINUE) : 0);
        }
        return ::SetServiceStatus(status_handle, &status) != 0;
    }

    // SCM 控制码（HandlerEx 签名，带 ctx）
    static DWORD WINAPI control_handler_static(DWORD code, DWORD event_type,
                                               LPVOID event_data, LPVOID ctx)
    {
        Impl* self = static_cast<Impl*>(ctx);
        (void)event_type;
        (void)event_data;
        switch (code) {
            case SERVICE_CONTROL_STOP:
            case SERVICE_CONTROL_SHUTDOWN:
                self->log("stop requested");
                self->stop_event = true;
                self->report(SERVICE_STOP_PENDING, NO_ERROR, 5000);
                return NO_ERROR;
            case SERVICE_CONTROL_PAUSE:
                if (self->on_pause) {
                    self->on_pause();
                    self->paused = true;
                    self->log("paused");
                    self->report(SERVICE_PAUSED, NO_ERROR, 0);
                }
                return NO_ERROR;
            case SERVICE_CONTROL_CONTINUE:
                if (self->on_resume) {
                    self->on_resume();
                    self->paused = false;
                    self->log("resumed");
                    self->report(SERVICE_RUNNING, NO_ERROR, 0);
                }
                return NO_ERROR;
            case SERVICE_CONTROL_INTERROGATE:
                self->report(self->status.dwCurrentState);
                return NO_ERROR;
            default:
                return NO_ERROR;  // 未实现的控制码按已接收处理
        }
    }

    // ServiceMain（LPSERVICE_MAIN_FUNCTIONW 签名，无 ctx → 用 s_active）
    static void WINAPI service_main_static(DWORD argc, LPWSTR* argv)
    {
        Impl* self = s_active;
        (void)argc;
        (void)argv;
        if (self == nullptr) {
            return;
        }

        self->status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        self->status_handle = ::RegisterServiceCtrlHandlerExW(
            internal::utf8_to_wide(self->name).c_str(),
            &Impl::control_handler_static, self);
        if (self->status_handle == NULL) {
            return;  // SCM 会在超时后回收
        }
        self->report(SERVICE_START_PENDING, NO_ERROR, 3000);

        self->log("starting");
        if (self->run_callback) {
            self->app_exit_code = self->run_callback(self->stop_event);
        }
        self->log("stopped");
        self->report(SERVICE_STOPPED,
                     static_cast<DWORD>(self->app_exit_code));
    }

    // 控制台 Ctrl+C 处理（调试模式）
    static BOOL WINAPI console_ctrl_static(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
            type == CTRL_CLOSE_EVENT) {
            if (s_active) {
                s_active->stop_event = true;
            }
            return TRUE;
        }
        return FALSE;
    }
};

ServiceApp::Impl* ServiceApp::Impl::s_active = nullptr;

ServiceApp::ServiceApp(std::string name, std::string display_name)
    : impl_(new Impl(std::move(name), std::move(display_name)))
{
    Impl::s_active = impl_;
}

ServiceApp::~ServiceApp()
{
    delete impl_;  // Impl 析构清 s_active
}

void ServiceApp::set_run_callback(ServiceMainFn callback)
{
    impl_->run_callback = std::move(callback);
}

void ServiceApp::set_description(const std::string& description)
{
    impl_->description = description;
}

void ServiceApp::set_start_type(ServiceStartType type)
{
    impl_->start_type = type;
}

void ServiceApp::set_log_callback(std::function<void(const char*)> on_log)
{
    impl_->log_callback = std::move(on_log);
}

void ServiceApp::set_pause_callbacks(ServicePauseFn on_pause, ServicePauseFn on_resume)
{
    impl_->on_pause = std::move(on_pause);
    impl_->on_resume = std::move(on_resume);
}

int ServiceApp::run(int argc, char* argv[])
{
    const std::string cmd = argc > 1 ? argv[1] : std::string();

    // SCM 调起服务进程时，argv[1] 是服务名（StartService 传入），
    // 此时无论参数是什么都进入服务模式
    const bool invoked_by_scm =
        (argc > 1 && impl_->name == argv[1]);

    if (cmd == "install" && !invoked_by_scm) {
        ServiceInstallDesc desc;
        desc.name = impl_->name;
        desc.display_name = impl_->display_name;
        desc.description = impl_->description;
        wchar_t exe[MAX_PATH];
        ::GetModuleFileNameW(NULL, exe, MAX_PATH);
        desc.bin_path = internal::wide_to_utf8(exe);
        desc.start_type = impl_->start_type;
        if (ServiceControl::install(desc)) {
            impl_->log("service installed");
            return 0;
        }
        impl_->log("install failed (need admin?)");
        return 1;
    }
    if (cmd == "uninstall" && !invoked_by_scm) {
        if (ServiceControl::uninstall(impl_->name)) {
            impl_->log("service uninstalled");
            return 0;
        }
        impl_->log("uninstall failed (need admin?)");
        return 1;
    }
    if (cmd == "start" && !invoked_by_scm) {
        if (ServiceControl::start(impl_->name) &&
            ServiceControl::wait_for_state(impl_->name, ServiceState::Running,
                                           10000)) {
            impl_->log("service started");
            return 0;
        }
        impl_->log("start failed");
        return 1;
    }
    if (cmd == "stop" && !invoked_by_scm) {
        if (ServiceControl::stop(impl_->name)) {
            impl_->log("service stopped");
            return 0;
        }
        impl_->log("stop failed");
        return 1;
    }

    // -------- 服务模式：SCM 分发器（阻塞到服务停止） --------
    if (cmd == "run" || invoked_by_scm) {
        std::wstring wname = internal::utf8_to_wide(impl_->name);
        SERVICE_TABLE_ENTRYW entry[2];
        entry[0].lpServiceName = const_cast<LPWSTR>(wname.c_str());
        entry[0].lpServiceProc = &Impl::service_main_static;
        entry[1].lpServiceName = NULL;
        entry[1].lpServiceProc = NULL;

        if (!::StartServiceCtrlDispatcherW(entry)) {
            // 错误 1063 = 未被 SCM 调起（用户手动执行了 run）
            if (::GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                impl_->log(
                    "run: not started by SCM; use 'console' for foreground "
                    "debug, or 'start' via admin");
            }
            return 1;
        }
        return impl_->app_exit_code;
    }

    // -------- 调试模式：前台运行，Ctrl+C 触发 stop --------
    if (cmd.empty() || cmd == "console") {
        impl_->log("console mode (Ctrl+C to stop)");
        ::SetConsoleCtrlHandler(&Impl::console_ctrl_static, TRUE);
        int exit_code = 0;
        if (impl_->run_callback) {
            exit_code = impl_->run_callback(impl_->stop_event);
        }
        impl_->log("console mode exit");
        return exit_code;
    }

    // 不认识的参数：交给宿主程序处理
    return -1;
}

#else  // 非 Windows 平台：桩实现（编译通过、运行时返回失败/直接运行回调）

bool ServiceControl::install(const ServiceInstallDesc&)
{
    return false;
}
bool ServiceControl::uninstall(const std::string&, int)
{
    return false;
}
bool ServiceControl::start(const std::string&)
{
    return false;
}
bool ServiceControl::stop(const std::string&, int)
{
    return false;
}
ServiceStatusInfo ServiceControl::query(const std::string&)
{
    ServiceStatusInfo out;
    out.state = ServiceState::Unknown;
    return out;
}
bool ServiceControl::query_start_type(const std::string&, ServiceStartType&)
{
    return false;
}
bool ServiceControl::set_start_type(const std::string&, ServiceStartType)
{
    return false;
}
bool ServiceControl::wait_for_state(const std::string&, ServiceState, int)
{
    return false;
}

struct ServiceApp::Impl
{
    std::string name;
    std::string display_name;
    std::string description;
    ServiceStartType start_type = ServiceStartType::AutoStart;
    ServiceMainFn run_callback;
    std::function<void(const char*)> log_callback;
    std::atomic<bool> stop_event{false};

    explicit Impl(std::string n, std::string dn)
        : name(std::move(n)), display_name(std::move(dn))
    {
    }
    void log(const char* msg)
    {
        if (log_callback) {
            log_callback(msg);
        } else {
            std::fprintf(stderr, "[service] %s\n", msg);
        }
    }
};

ServiceApp::ServiceApp(std::string name, std::string display_name)
    : impl_(new Impl(std::move(name), std::move(display_name)))
{
}
ServiceApp::~ServiceApp() { delete impl_; }
void ServiceApp::set_run_callback(ServiceMainFn callback)
{
    impl_->run_callback = std::move(callback);
}
void ServiceApp::set_description(const std::string& d)
{
    impl_->description = d;
}
void ServiceApp::set_start_type(ServiceStartType t)
{
    impl_->start_type = t;
}
void ServiceApp::set_log_callback(std::function<void(const char*)> cb)
{
    impl_->log_callback = std::move(cb);
}
int ServiceApp::run(int argc, char* argv[])
{
    const std::string cmd = argc > 1 ? argv[1] : std::string();
    // 非 Windows：仅支持 console 语义；未知参数交还宿主
    if (!cmd.empty() && cmd != "console") {
        return -1;
    }
    if (impl_->run_callback) {
        return impl_->run_callback(impl_->stop_event);
    }
    return 0;
}

#endif  // _WIN32

}  // namespace libmini
