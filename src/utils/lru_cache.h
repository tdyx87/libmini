#ifndef LIBMINI_LRU_CACHE_H
#define LIBMINI_LRU_CACHE_H

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>

#include "libmini.h"
#include "optional.h"

namespace libmini {

// 线程安全的 LRU 缓存（容量上限 + 命中率统计）。
// 典型用途：RPC 结果缓存、文件元数据缓存、去重表。
//
//   LruCache<std::string, int> cache(1000);
//   cache.put("a", 1);
//   auto v = cache.get("a");            // optional：命中返回值并提升到最新
//   if (!v) { v = compute(); cache.put("a", *v); }
//   double hit = cache.hit_rate();      // 命中率（自创建或 reset_stats 起）
//
// put 已存在的键：更新值并提升为最新，容量不变。
// 容量为 0：退化为不缓存（put 直接丢弃，get 永远 miss）。
template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(std::size_t capacity)
        : capacity_(capacity)
    {
    }

    // 取值：命中返回值并把该项提升为最新；未命中返回空 optional
    optional<V> get(const K& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++lookups_;
        auto it = index_.find(key);
        if (it == index_.end()) {
            return nullopt;
        }
        ++hits_;
        // 提升到链表头（最新端）
        items_.splice(items_.begin(), items_, it->second);
        return it->second->value;
    }

    // 放入/更新：已存在的键更新值并提升为最新；新键插入头部，
    // 超过容量时淘汰最旧（链表尾）
    void put(const K& key, V value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = std::move(value);
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        if (capacity_ == 0) {
            return;  // 不缓存
        }
        items_.push_front(Item{key, std::move(value)});
        index_[key] = items_.begin();
        if (items_.size() > capacity_) {
            index_.erase(items_.back().key);
            items_.pop_back();
        }
    }

    // 显式删除（如缓存失效场景）；返回是否确实删除了
    bool erase(const K& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        items_.erase(it->second);
        index_.erase(it);
        return true;
    }

    // 当前条目数
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty();
    }

    // 命中率统计（lookups 含命中与未命中）
    double hit_rate() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lookups_ == 0 ? 0.0
                             : static_cast<double>(hits_) /
                                   static_cast<double>(lookups_);
    }

    std::size_t lookups() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lookups_;
    }

    // 清空缓存内容；clear_stats=true 时命中率计数一并清零
    void clear(bool clear_stats = false)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
        index_.clear();
        if (clear_stats) {
            lookups_ = 0;
            hits_ = 0;
        }
    }

private:
    struct Item
    {
        K key;
        V value;
    };

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::list<Item> items_;  // 头部 = 最新，尾部 = 最旧
    std::unordered_map<K, typename std::list<Item>::iterator> index_;
    std::size_t lookups_ = 0;
    std::size_t hits_ = 0;
};

}  // namespace libmini

#endif  // LIBMINI_LRU_CACHE_H
