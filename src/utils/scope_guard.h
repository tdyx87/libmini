#ifndef LIBMINI_SCOPE_GUARD_H
#define LIBMINI_SCOPE_GUARD_H

#include <utility>

#include "libmini.h"

namespace libmini {

// RAII 作用域守卫（对应 boost::scope / ScopeGuard 惯用法）：
// 构造时捕获一个可调用对象，析构时（或 dismiss 后不）执行。
// 典型用途：错误路径上保证资源释放、mutex 忘记解锁防护等。
//
//   FILE* f = fopen("a.txt", "r");
//   auto guard = make_scope_guard([f] { if (f) fclose(f); });
//   if (!read_all(f)) return;   // guard 析构自动 fclose
//   guard.dismiss();            // 成功路径：取消清理
//
// ScopeGuard 不可拷贝，可移动（移动后源对象失效）。
// 模板类在使用方编译单元内实例化，无需导出宏。
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F f) : f_(std::move(f)), dismissed_(false) {}

    ScopeGuard(ScopeGuard&& other)
        : f_(std::move(other.f_)), dismissed_(other.dismissed_)
    {
        other.dismissed_ = true;  // 移动后源对象不再执行
    }

    ScopeGuard& operator=(ScopeGuard&&) = delete;
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

    ~ScopeGuard()
    {
        if (!dismissed_) {
            f_();
        }
    }

    // 取消析构时执行的清理动作
    void dismiss() { dismissed_ = true; }

private:
    F f_;
    bool dismissed_;
};

// 推导辅助函数（C++11 无 CTAD，需要显式模板参数时用它）
template <typename F>
ScopeGuard<typename std::decay<F>::type> make_scope_guard(F&& f)
{
    return ScopeGuard<typename std::decay<F>::type>(std::forward<F>(f));
}

}  // namespace libmini

#endif  // LIBMINI_SCOPE_GUARD_H
