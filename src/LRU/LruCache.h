#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

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

    int capacity_;              // 缓存容量
    NodeMap nodeMap_;           // 存储所有节点的映射，方便快速查找节点
    mutable std::mutex mutex_;  // size() 为 const，需 mutable 才能加锁
    NodePtr dummyHead_;
    NodePtr dummyTail_;

   public:
    LruCache(int capacity) : capacity_(capacity) { initializeList(); }

    // 如果缓存项存在，更新并移动到最新位置
    void put(Key key, Value value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end()) {
            updateExistingNode(it->second, value);
        } else {
            addNewNode(key, value);
        }
    }

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

    void remove(const Key& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end()) {
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

    // 获取缓存项数量
    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return nodeMap_.size();
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
    // LRU淘汰策略再次体现：如果缓存已满，驱逐最近最少访问的节点
    void addNewNode(const Key& key, const Value& value) {
        if (nodeMap_.size() >= capacity_) {
            evictLeastRecent();
        }

        auto node = std::make_shared<Node>(key, value);
        insertNode(node);
        nodeMap_[key] = node;
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

    // 逻辑移除
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

    // 从尾部移除，物理移除（包括映射）
    void evictLeastRecent() {
        auto leastRecent = dummyTail_->prev_.lock();
        // 排除虚拟头节点
        if (leastRecent == dummyHead_) {
            return;
        }

        removeNode(leastRecent);
        // 从map中移除
        nodeMap_.erase(leastRecent->key);
    }
};
}  // namespace louis::cache