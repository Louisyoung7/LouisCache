#pragma once
#include <functional>

namespace louis::cache {
enum class LeaveReason { Evicted, Explicit, Expired };

template <typename Key, typename Value>
class Policy {
   public:
    using LeaveCallback =
        std::function<void(const Key& key, const Value& value, LeaveReason reason)>;
    // 虚析构
    virtual ~Policy() = default;

    // 添加缓存项
    virtual void put(Key key, Value value) = 0;

    // 查询缓存项
    // value是传出参数
    virtual bool get(const Key& key, Value& value) = 0;

    // 查询缓存项，返回值
    virtual Value get(const Key& key) = 0;

    // 移除缓存项
    virtual void remove(const Key& key) = 0;

    // 设置缓存项移除回调
    void setLeaveCallback(LeaveCallback cb) { leaveCallback_ = std::move(cb); }

    // 获取缓存项数量
    virtual size_t size() const = 0;

    // 查询是否存在（peek 语义：不刷新 recency、不触发淘汰、过期条目返回 false）
    virtual bool exists(const Key& key) const = 0;

    // 用户设定的容量（ARC 返回 T1+T2 主缓存总容量，Sharded 返回分片总量）
    virtual size_t capacity() const = 0;

   protected:
    // 通知缓存项移除回调
    void notifyLeave(const Key& k, const Value& v, LeaveReason r) {
        if (leaveCallback_) leaveCallback_(k, v, r);
    }

   private:
    LeaveCallback leaveCallback_;
};
}  // namespace louis::cache