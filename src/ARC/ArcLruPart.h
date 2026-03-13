#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "ARC/ArcNode.h"

namespace louis::cache {
template <typename Key, typename Value>
class ArcLruPart {
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    size_t capacity_;
    size_t ghostCapacity_;

    size_t transformThreshold_;  // 转换阈值

    std::mutex mutex_;

    NodeMap mainCacheMap_;
    NodeMap ghostCacheMap_;

    NodePtr mainHead_;
    NodePtr mainTail_;

    NodePtr ghostHead_;
    NodePtr ghostTail_;

   public:
    ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity), ghostCapacity_(capacity), transformThreshold_(transformThreshold) {
        initializeLists();
    }

    // 增加或更新缓存项
    bool put(Key key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCacheMap_.find(key);
        if (it != mainCacheMap_.end()) {
            return updateExistingNode(it->second, value);
        }

        return addNewNode(key, value);
    }

    // 获取缓存项，通过传出参数确定是否需要转换到LFU部分
    bool get(const Key& key, Value& value, bool& shouldTransform) {
        auto it = mainCacheMap_.find(key);
        if (it != mainCacheMap_.end()) {
            value = it->second->getValue();
            moveToFront(it->second);
            shouldTransform = updateNodeAccess(it->second);
            return true;
        }
        return false;
    }

    // 在幽灵缓存链表中检查指定键，并删除指定缓存项
    bool checkGhost(Key key) {
        auto it = ghostCacheMap_.find(key);
        if (it != ghostCacheMap_.end()) {
            removeFromGhost(it->second);
            ghostCacheMap_.erase(it);
            return true;
        }

        return false;
    }

    // 增加主缓存容量
    void increaseCapacity() {
        ++capacity_;
    }

    // 减少主缓存容量
    bool decreaseCapacity() {
        if (capacity_ <= 0) {
            return false;
        }

        // 如果缓存已满，要先移除最旧的缓存项
        if (mainCacheMap_.size() == capacity_) {
            evictLeastRecent();
        }

        --capacity_;
        return true;
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

    bool updateExistingNode(NodePtr node, const Value& value) {
        node->setValue(value);
        updateNodeAccess(node);
        moveToFront(node);
        return true;
    }

    // 添加新节点到主缓存
    bool addNewNode(const Key& key, const Value& value) {
        if (mainCacheMap_.size() >= capacity_) {
            evictLeastRecent();
        }

        auto node = std::make_shared<NodeType>(key, value);
        addToFront(node);
        mainCacheMap_[key] = node;
        return true;
    }

    // 更新节点访问次数，并返回是否需要转换到LFU部分
    bool updateNodeAccess(NodePtr node) {
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;
    }

    // 将主缓存链表中已存在的节点移动到头部
    void moveToFront(NodePtr node) {
        // 先从当前位置移除
        removeFromMain(node);
        // 再将节点重新添加
        addToFront(node);
    }

    // 将节点添加到主缓存链表头部
    void addToFront(NodePtr node) {
        if (node == nullptr) {
            return;
        }

        auto prev = mainHead_;
        auto next = mainHead_->next_;

        prev->next_ = node;
        node->prev_ = prev;

        node->next_ = next;
        next->prev_ = node;
    }

    // 从主缓存链表驱逐最旧节点，并移动到幽灵缓存链表
    void evictLeastRecent() {
        auto oldest = mainTail_->prev_.lock();

        if (oldest == mainHead_ || oldest == nullptr) {
            return;
        }

        // 从主缓存链表移除
        removeFromMain(oldest);
        mainCacheMap_.erase(oldest->getKey());

        if (ghostCacheMap_.size() >= ghostCapacity_) {
            removeOldestGhost();
        }

        // 将节点添加到幽灵缓存链表
        addToGhost(oldest);
    }

    // 从主缓存链表移除节点
    void removeFromMain(NodePtr node) {
        if (node->prev_.expired() || node->next_ == nullptr) {
            return;
        }

        auto prev = node->prev_.lock();
        auto next = node->next_;

        prev->next_ = next;
        next->prev_ = prev;

        node->prev_.reset();
        node->next_ = nullptr;
    }

    // 将节点添加到幽灵缓存链表
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

        ghostCacheMap_[node->getKey()] = node;
    }

    // 从幽灵缓存链表移除节点
    void removeFromGhost(NodePtr node) {
        if (node->prev_.expired() || node->next_ == nullptr) {
            return;
        }

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

        if (oldest == ghostHead_ || !oldest) {
            return;
        }

        removeFromGhost(oldest);
        ghostCacheMap_.erase(oldest->getKey());
    }
};
}  // namespace louis::cache