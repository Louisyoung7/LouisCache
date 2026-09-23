#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>

#include "ARC/ArcNode.h"

namespace louis::cache {
// 内部组件：不自行加锁，线程安全由调用方（ArcCache）的锁保证
template <typename Key, typename Value>
class ArcLruPart {
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

   public:
    ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity), ghostCapacity_(capacity), transformThreshold_(transformThreshold) {
        initializeLists();
    }

    // 增加或更新缓存项
    void put(Key key, Value value, bool& shouldTransform) {
        shouldTransform = false;
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) {
            updateExistingNode(it->second, value, shouldTransform);
        } else {
            addNewNode(key, value);
        }
    }

    // 获取缓存项，通过传出参数确定是否需要转换到LFU部分
    bool get(const Key& key, Value& value, bool& shouldTransform) {
        shouldTransform = false;
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) {
            value = it->second->getValue();
            moveToMostRecent(it->second);
            shouldTransform = updateNodeAccess(it->second);
            return true;
        }
        return false;
    }

    void remove(const Key& key) {
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) {
            removeFromMain(it->second);
            mainCache_.erase(it);
        }
    }

    size_t size() const { return mainCache_.size(); }

    // 检查主缓存是否包含指定键
    bool contain(const Key& key) { return mainCache_.find(key) != mainCache_.end(); }

    // 增加主缓存容量
    void increaseCapacity() { ++capacity_; }

    // 减少主缓存容量
    bool decreaseCapacity() {
        if (capacity_ <= 0) return false;

        // 如果缓存已满，要先移除最旧的缓存项
        if (mainCache_.size() == capacity_) evictLeastRecent();

        --capacity_;
        return true;
    }

    // 在幽灵缓存链表中检查指定键，并删除指定缓存项
    bool tryToRemoveGhost(Key key) {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

   private:
    // 初始化主缓存链表和幽灵缓存链表
    void initializeLists() {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    void updateExistingNode(NodePtr node, const Value& value, bool& shouldTransform) {
        node->setValue(value);
        shouldTransform = updateNodeAccess(node);
        moveToMostRecent(node);
    }

    void addNewNode(const Key& key, const Value& value) {
        if (mainCache_.size() >= capacity_) evictLeastRecent();

        auto node = std::make_shared<NodeType>(key, value);
        addToMain(node);
        mainCache_[key] = node;
    }

    // 更新节点访问次数，并返回是否需要转换到LFU部分
    bool updateNodeAccess(NodePtr node) {
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;
    }

    // 将主缓存链表中已存在的节点移动到头部
    void moveToMostRecent(NodePtr node) {
        removeFromMain(node);
        addToMain(node);
    }

    // 将节点添加到主缓存链表头部
    void addToMain(NodePtr node) {
        if (node == nullptr) return;

        auto prev = mainHead_;
        auto next = mainHead_->next_;

        prev->next_ = node;
        node->prev_ = prev;

        node->next_ = next;
        next->prev_ = node;
    }

    // 从主缓存链表移除节点
    void removeFromMain(NodePtr node) {
        if (node->prev_.expired() || node->next_ == nullptr) return;

        auto prev = node->prev_.lock();
        auto next = node->next_;

        prev->next_ = next;
        next->prev_ = prev;

        node->prev_.reset();
        node->next_ = nullptr;
    }

    // 从主缓存链表驱逐最旧节点，并移动到幽灵缓存链表
    void evictLeastRecent() {
        auto oldest = mainTail_->prev_.lock();
        if (oldest == mainHead_ || oldest == nullptr) return;

        // 从主缓存链表移除
        removeFromMain(oldest);
        mainCache_.erase(oldest->getKey());

        if (ghostCache_.size() >= ghostCapacity_) removeOldestGhost();

        // 将节点添加到幽灵缓存链表
        addToGhost(oldest);
    }

    /// Ghost Cache 操作

    void addToGhost(NodePtr node) {
        // 重置节点访问次数
        node->resetAccessCount();

        // 添加到头部
        auto prev = ghostHead_;
        auto next = ghostHead_->next_;

        prev->next_ = node;
        node->prev_ = prev;

        node->next_ = next;
        next->prev_ = node;

        ghostCache_[node->getKey()] = node;
    }

    void removeFromGhost(NodePtr node) {
        if (node->prev_.expired() || node->next_ == nullptr) return;

        auto prev = node->prev_.lock();
        auto next = node->next_;

        prev->next_ = next;
        next->prev_ = prev;

        node->prev_.reset();
        node->next_ = nullptr;
    }

    // 从幽灵缓存链表驱逐最旧节点
    void removeOldestGhost() {
        auto oldest = ghostTail_->prev_.lock();
        if (oldest == ghostHead_ || !oldest) return;

        removeFromGhost(oldest);
        ghostCache_.erase(oldest->getKey());
    }

    size_t capacity_;
    NodeMap mainCache_;
    NodePtr mainHead_;  // LRU
    NodePtr mainTail_;

    size_t ghostCapacity_;
    NodeMap ghostCache_;
    NodePtr ghostHead_;  // LRU
    NodePtr ghostTail_;

    size_t transformThreshold_;  // 转换阈值
};
}  // namespace louis::cache