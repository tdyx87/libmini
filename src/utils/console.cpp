#include "console.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <csignal>
#include <cstring>
#endif

namespace libmini {

struct ConsoleExit::Impl
{
    std::function<void()> handler;
    std::mutex mutex;
    std::condition_variable done_cv;
    bool handler_done = false;
#ifdef _WIN32
#else
    std::thread wait_thread;
#endif
};

// 控制处理器桥：持有属主指针，信号到达时置位并执行回调。
// ConsoleExit 把 Impl 声明为 friend，桥接函数通过成员指针访问
namespace {

ConsoleExit* g_console_exit = nullptr;

}  // namespace

BOOL WINAPI console_ctrl_handler(DWORD type)
{
    (void)type;  // CTRL_C_EVENT / CTRL_BREAK_EVENT 等统一按退出处理
    if (g_console_exit != nullptr) {
        g_console_exit->on_signal();
        return TRUE;
    }
    return FALSE;
}

ConsoleExit::ConsoleExit()
    : impl_(new Impl)
{
#ifdef _WIN32
    g_console_exit = this;
    ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    // POSIX：阻塞相关信号后开专用线程 sigwait——处理逻辑不占用
    // 信号上下文，比 async-signal-safe 回调简单可靠得多
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    ::sigaddset(&set, SIGHUP);
    ::pthread_sigmask(SIG_BLOCK, &set, NULL);

    wait_thread = std::thread([this, set]() {
        int sig = 0;
        for (;;) {
            if (::sigwait(&set, &sig) == 0) {
                break;
            }
        }
        on_signal();
    });
#endif
}

ConsoleExit::~ConsoleExit()
{
#ifdef _WIN32
    ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    if (g_console_exit == this) {
        g_console_exit = nullptr;
    }
#else
    // 等待线程退出（不发信号则 detached 掉，避免析构挂死）
    if (wait_thread.joinable()) {
        if (stop_requested_.load()) {
            wait_thread.join();
        } else {
            wait_thread.detach();
        }
    }
#endif
    delete impl_;
}

void ConsoleExit::set_handler(std::function<void()> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->handler = std::move(handler);
}

// 信号到达的统一入口（控制处理器 / sigwait 线程都调这里）。
// 公有成员，桥接函数无障碍访问；重复信号只处理一次
void ConsoleExit::on_signal()
{
    if (stop_requested_.exchange(true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->handler) {
            impl_->handler();  // 在系统派发的线程上执行
        }
        impl_->handler_done = true;
    }
    impl_->done_cv.notify_all();
    // Windows：返回 TRUE 让系统暂不强制终止，给回调收尾的机会
}

void ConsoleExit::wait()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->done_cv.wait(lock, [this]() { return impl_->handler_done; });
}

}  // namespace libmini
