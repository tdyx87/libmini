#ifndef LIBMINI_OPTIONAL_H
#define LIBMINI_OPTIONAL_H

#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "libmini.h"

namespace libmini {

// 空 optional 的标记类型
struct nullopt_t {
    explicit nullopt_t(int) {}
};
const nullopt_t nullopt{0};

// 访问空 optional 的 value() 时抛出
class bad_optional_access : public std::exception {
public:
    const char* what() const noexcept override
    {
        return "libmini::bad_optional_access: optional is empty";
    }
};

// C++11 版 optional（对应 boost::optional / C++17 std::optional）。
//   optional<int> parse(const std::string& s);
//   if (auto v = parse("42")) use(*v);
//   int n = parse("x").value_or(-1);
template <typename T>
class optional {
public:
    // 空值
    optional() : engaged_(false) {}
    optional(nullopt_t) : engaged_(false) {}

    // 带值构造
    optional(const T& value) : engaged_(false)
    {
        construct(value);
    }
    optional(T&& value) : engaged_(false)
    {
        construct(std::move(value));
    }

    optional(const optional& other) : engaged_(false)
    {
        if (other.engaged_) {
            construct(*other.val());
        }
    }

    optional(optional&& other) : engaged_(false)
    {
        if (other.engaged_) {
            construct(std::move(*other.val()));
            other.reset();
        }
    }

    optional& operator=(nullopt_t)
    {
        reset();
        return *this;
    }

    optional& operator=(const T& value)
    {
        if (engaged_) {
            *val() = value;
        } else {
            construct(value);
        }
        return *this;
    }

    optional& operator=(T&& value)
    {
        if (engaged_) {
            *val() = std::move(value);
        } else {
            construct(std::move(value));
        }
        return *this;
    }

    optional& operator=(const optional& other)
    {
        if (this == &other) {
            return *this;
        }
        if (engaged_ && other.engaged_) {
            *val() = *other.val();
        } else if (other.engaged_) {
            reset();
            construct(*other.val());
        } else {
            reset();
        }
        return *this;
    }

    optional& operator=(optional&& other)
    {
        if (this == &other) {
            return *this;
        }
        if (engaged_ && other.engaged_) {
            *val() = std::move(*other.val());
        } else if (other.engaged_) {
            reset();
            construct(std::move(*other.val()));
        } else {
            reset();
        }
        return *this;
    }

    ~optional()
    {
        reset();
    }

    // 原地构造值
    template <typename... Args>
    T& emplace(Args&&... args)
    {
        reset();
        ::new (static_cast<void*>(&storage_)) T(std::forward<Args>(args)...);
        engaged_ = true;
        return *val();
    }

    // 清空
    void reset()
    {
        if (engaged_) {
            val()->~T();
            engaged_ = false;
        }
    }

    bool has_value() const { return engaged_; }
    explicit operator bool() const { return engaged_; }

    // 访问值（空时为未定义行为，先用 has_value()/operator bool 判断）
    T& operator*() { return *val(); }
    const T& operator*() const { return *val(); }
    T* operator->() { return val(); }
    const T* operator->() const { return val(); }

    T& value()
    {
        if (!engaged_) {
            throw bad_optional_access();
        }
        return *val();
    }
    const T& value() const
    {
        if (!engaged_) {
            throw bad_optional_access();
        }
        return *val();
    }

    // 空时返回 fallback
    T value_or(T fallback) const
    {
        return engaged_ ? *val() : fallback;
    }

    void swap(optional& other)
    {
        if (engaged_ && other.engaged_) {
            using std::swap;
            swap(*val(), *other.val());
        } else if (engaged_) {
            other.construct(std::move(*val()));
            reset();
        } else if (other.engaged_) {
            construct(std::move(*other.val()));
            other.reset();
        }
    }

private:
    template <typename U>
    void construct(U&& value)
    {
        ::new (static_cast<void*>(&storage_)) T(std::forward<U>(value));
        engaged_ = true;
    }

    T* val() { return static_cast<T*>(static_cast<void*>(&storage_)); }
    const T* val() const
    {
        return static_cast<const T*>(static_cast<const void*>(&storage_));
    }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
    bool engaged_;
};

// 推导辅助
template <typename T>
optional<typename std::decay<T>::type> make_optional(T&& value)
{
    return optional<typename std::decay<T>::type>(std::forward<T>(value));
}

}  // namespace libmini

#endif  // LIBMINI_OPTIONAL_H
