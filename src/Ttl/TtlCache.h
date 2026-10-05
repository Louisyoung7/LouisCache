#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "Policy.h"

namespace louis::cache {

template <typename Key, typename Value, typename Clock = std::chrono::steady_clock>
class TtlCache : public Policy<Key, Value> {
    using TimePoint = typename Clock::time_point;
    using Duration = typename Clock::duration;

   public:
    TtlCache(std::shared_ptr<Policy<Key, Value>> inner, Duration ttl) : inner_(std::move(inner)), ttl_(ttl) {
        if (!inner_) throw std::invalid_argument("inner cache is null");

        inner_->setLeaveCallback([this](const Key& k, const Value& v, LeaveReason r) {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            pendingInnerLeaves_.emplace_back(k, v, r);
        });
    }

    // 禁止拷贝和移动
    // inner_ 回调捕获本对象 this
    // 拷贝会使 this 路由混乱，移动会使闭包内指针悬垂
    // 故禁止拷贝/移动
    TtlCache(const TtlCache&) = delete;
    TtlCache& operator=(const TtlCache&) = delete;
    TtlCache(TtlCache&&) = delete;
    TtlCache& operator=(TtlCache&&) = delete;

    void put(Key key, Value value) override {
        auto now = Clock::now();  // 记录当前时间
        std::vector<LeaveRecord> outward;
        std::unordered_map<Key, Value> expiredVictims;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            expiryMap_[key] = now + ttl_;
            inner_->put(key, std::move(value));  // 可能缓冲 Evicted 项
            outward = drainAndClassifyLocked(now, expiredVictims);
            // 容量为0时，inner 未真正驻留缓存项，直接从 expiryMap_ 中移除，无回调触发
            if (!inner_->exists(key)) expiryMap_.erase(key);
        }
        for (auto& e : outward) this->notifyLeave(e.key, std::move(e.value), e.reason);
    }

    bool get(Key key, Value& value) override {
        auto now = Clock::now();  // 记录当前时间
        std::vector<LeaveRecord> outward;
        std::unordered_map<Key, Value> expiredVictims;
        // 尝试从 inner 缓存中清除单个过期项，可能缓冲需要处理的 Explicit 项
        purgeKeyIfExpiredLocked(key, now, expiredVictims);
        bool isHit = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            isHit = inner_->get(key, value);  // 可能缓冲 Evicted 项
            outward = drainAndClassifyLocked(now, expiredVictims);
        }
        for (auto& e : outward) this->notifyLeave(e.key, std::move(e.value), e.reason);
        return isHit;
    }

    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

    void remove(const Key& key) override {
        auto now = Clock::now();  // 记录当前时间
        std::vector<LeaveRecord> outward;
        std::unordered_map<Key, Value> expiredVictims;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inner_->remove(key);  // 缓冲 Explicit 项
            // 这里 expiredVictims 为空，没有需要处理的过期项
            outward = drainAndClassifyLocked(now, expiredVictims);
        }
        for (auto& e : outward) this->notifyLeave(e.key, std::move(e.value), e.reason);
    }

    size_t size() const override {
        auto now = Clock::now();  // 记录当前时间
        std::vector<LeaveRecord> outward;
        std::unordered_map<Key, Value> expiredVictims;
        // 先清除所有过期项，所有的过期项都会缓冲需要处理的 Explicit 项
        purgeExpiredLocked(now, expiredVictims);
        // 再统计当前缓存项数量
        size_t size = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            size = inner_->size();
            outward = drainAndClassifyLocked(now, expiredVictims);
        }
        for (auto& e : outward) this->notifyLeave(e.key, std::move(e.value), e.reason);
        return size;
    }

    bool exists(const Key& key) const override {
        auto now = Clock::now();  // 记录当前时间
        std::vector<LeaveRecord> outward;
        std::unordered_map<Key, Value> expiredVictims;
        // 尝试从 inner 缓存中清除单个过期项，可能缓冲需要处理的 Explicit 项
        purgeKeyIfExpiredLocked(key, now, expiredVictims);
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exists = inner_->exists(key);
            outward = drainAndClassifyLocked(now, expiredVictims);
        }
        for (auto& e : outward) this->notifyLeave(e.key, std::move(e.value), e.reason);
        return exists;
    }

    size_t capacity() const override {
        // 容量无法运行时修改，恒线程安全
        return inner_->capacity();
    }

    size_t purgeExpired() {
        size_t rawSize = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rawSize = inner_->size();
        }
        size_t clearSize = size();
        return rawSize - clearSize;
    }

   private:
    struct LeaveRecord {
        Key key;
        Value value;
        LeaveReason reason;
    };

    // 检查 key 是否过期
    // 如果过期，将其从 inner 缓存中移除
    // 不在此处从 expiryMap_ 中移除，避免迭代器失效
    bool purgeKeyIfExpiredLocked(
        const Key& key, TimePoint now, std::unordered_map<Key, Value>& expiredVictims
    ) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expiryMap_.find(key) == expiryMap_.end()) return false;
        if (expiryMap_[key] > now) return false;

        Value value;
        if (!inner_->get(key, value)) return false;  // 一般不会失败

        expiredVictims.emplace(key, std::move(value));

        inner_->remove(key);  // 从 inner 缓存中移除，触发 inner 移除回调， r = Explicit

        return true;
    }

    // 将所有过期键值对从 inner 缓存中移除
    void purgeExpiredLocked(TimePoint now, std::unordered_map<Key, Value>& expiredVictims) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, expiry] : expiryMap_) {
            if (expiry > now) continue;
            purgeKeyIfExpiredLocked(key, now, expiredVictims);
        }
    }
    // 由于 expiredVictims 统一为局部数据，所以不为空一定要用此方法及时处理
    std::vector<LeaveRecord> drainAndClassifyLocked(
        TimePoint now, std::unordered_map<Key, Value>& expiredVictims
    ) {
        std::vector<LeaveRecord> outward;
        // 先处理 expiredVictims

        for (auto& [key, value] : expiredVictims) {
            outward.emplace_back(key, std::move(value), LeaveReason::Explicit);
            expiryMap_.erase(key);
        }
        // 再消化 pendingInnerLeaves_
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);

            for (auto& [key, value, reason] : pendingInnerLeaves_) {
                if (expiredVictims.find(key) != expiredVictims.end()) {
                    // 阶段一已处理
                    continue;
                } else if (reason == LeaveReason::Explicit) {
                    // 用户显式删除，即使条目恰好过期也报 Explicit
                    outward.emplace_back(key, value, reason);
                    expiryMap_.erase(key);
                } else if (reason == LeaveReason::Evicted) {
                    // 条目被驱逐，先检查是否过期
                    bool zombie = expiryMap_.find(key) != expiryMap_.end() && expiryMap_[key] <= now;
                    expiryMap_.erase(key);
                    outward.emplace_back(key, value, zombie ? LeaveReason::Expired : reason);
                }
            }
        }
        return outward;
    }

    std::shared_ptr<Policy<Key, Value>> inner_;
    Duration ttl_;
    std::unordered_map<Key, TimePoint> expiryMap_;  // 装饰器自持过期表
    mutable std::mutex mutex_;
    std::vector<LeaveRecord> pendingInnerLeaves_;  // inner 回调缓冲
    std::mutex bufferMutex_;                       // 叶子锁
};
}  // namespace louis::cache