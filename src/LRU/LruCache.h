#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value>
class LruCache : public Policy<Key, Value> {
    struct Node {
        Key key;
        Value value;
        std::weak_ptr<Node> prev_;
        std::shared_ptr<Node> next_;
        Node(Key k, Value v) : key(std::move(k)), value(std::move(v)) {}
    };

    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

   public:
    LruCache(int capacity) : capacity_(capacity) { initializeList(); }

    // 插入或更新缓存项
    // 如果缓存项存在，更新并移动到最新位置
    // 如果缓存项不存在，添加新节点
    // 如果缓存已满，驱逐最近最少访问的节点
    void put(Key key, Value value) override {
        if (capacity_ <= 0) return;  // 容量 0：无处可存，直接丢弃（条目未曾驻留，不通知）
        std::optional<std::pair<Key, Value>> evicted;  // 锁内收集，锁外分发
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = nodeMap_.find(key);
            if (it != nodeMap_.end()) {
                updateExistingNode(it->second, value);
            } else {
                evicted = addNewNode(key, value);
            }
        }
        if (evicted) {
            this->notifyLeave(evicted->first, evicted->second, LeaveReason::Evicted);
        }
    }

    // 尝试获取缓存项，如果存在则移动到最新位置
    bool get(const Key& key, Value& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end()) {
            moveToMostRecent(it->second);
            value = it->second->value;
            return true;
        }
        return false;
    }

    Value get(const Key& key) override {
        Value value{};
        get(key, value);
        return value;
    }

    // 显式移除缓存项
    void remove(const Key& key) override {
        Value value{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = nodeMap_.find(key);
            if (it == nodeMap_.end()) return;

            value = std::move(it->second->value);
            removeNode(it->second);
            nodeMap_.erase(it);
        }
        // notifyLeave 位于依赖基类 Policy<Key, Value> 中，需 this-> 才能在实例化时找到
        this->notifyLeave(key, value, LeaveReason::Explicit);
    }

    // 获取缓存项数量
    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return nodeMap_.size();
    }

    // 查询缓存项是否存在
    // 不触发淘汰策略
    std::optional<Value> contains(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it == nodeMap_.end()) return std::nullopt;
        return it->second->value;
    }

   private:
    // 初始化虚拟首尾节点
    void initializeList() {
        dummyHead_ = std::make_shared<Node>(Key(), Value());
        dummyTail_ = std::make_shared<Node>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    // 更新后的节点会被移动到最新的位置
    void updateExistingNode(NodePtr node, const Value& value) {
        node->value = value;
        moveToMostRecent(node);
    }

    // 添加新节点
    // LRU淘汰策略体现：如果缓存已满，驱逐最近最少访问的节点
    // 调用方需持有锁；返回被驱逐的条目（未驱逐则返回空）
    std::optional<std::pair<Key, Value>> addNewNode(const Key& key, const Value& value) {
        std::optional<std::pair<Key, Value>> evicted;
        if (nodeMap_.size() >= capacity_) evicted = evictLeastRecent();

        auto node = std::make_shared<Node>(key, value);
        insertNode(node);
        nodeMap_[key] = node;
        return evicted;
    }

    // 先移除再添加
    void moveToMostRecent(NodePtr node) {
        removeNode(node);
        insertNode(node);
    }

    // 添加到头部
    void insertNode(NodePtr node) {
        auto next = dummyHead_->next_;

        dummyHead_->next_ = node;
        node->prev_ = dummyHead_;

        node->next_ = next;
        next->prev_ = node;
    }

    // 从链表中移除节点
    void removeNode(NodePtr node) {
        if (node->prev_.expired() || node->next_ == nullptr) {
            return;
        }

        auto prev = node->prev_.lock();
        auto next = node->next_;

        prev->next_ = next;
        next->prev_ = prev;

        node->next_ = nullptr;
        node->prev_.reset();
    }

    // 驱逐最近最少访问的节点
    // 调用方需持有锁；只做数据结构变更，不触发回调，由上层在锁外分发
    std::optional<std::pair<Key, Value>> evictLeastRecent() {
        auto leastRecent = dummyTail_->prev_.lock();
        // 排除虚拟头节点
        if (leastRecent == dummyHead_) return std::nullopt;

        auto key = leastRecent->key;
        auto value = std::move(leastRecent->value);

        removeNode(leastRecent);
        nodeMap_.erase(key);
        return std::make_pair(std::move(key), std::move(value));
    }

    int capacity_;              // 缓存容量
    NodeMap nodeMap_;           // 存储所有节点的映射，方便快速查找节点
    mutable std::mutex mutex_;  // size() 为 const，需 mutable 才能加锁
    NodePtr dummyHead_;
    NodePtr dummyTail_;
};
}  // namespace louis::cache