#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ARC/ArcNode.h"

namespace louis::cache {
// 内部组件：不自行加锁，线程安全由调用方（ArcCache）的锁保证
template <typename Key, typename Value>
class ArcLruPart {
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

   public:
    // 空构造：容量（主缓存与幽灵缓存）不在此处给定，
    // 由 ArcCache::applyPartitions 按自适应参数 p 统一设置
    explicit ArcLruPart(size_t transformThreshold)
        : capacity_(0), ghostCapacity_(0), transformThreshold_(transformThreshold) {
        initializeLists();
    }

    // 增加或更新缓存项
    // 返回为腾出空间而驱逐的节点（已移入幽灵缓存，shared_ptr 仍有效）；无驱逐返回 nullptr
    NodePtr put(Key key, Value value, bool& shouldTransform) {
        shouldTransform = false;
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) {
            updateExistingNode(it->second, value, shouldTransform);
            return nullptr;
        }
        return addNewNode(key, value);
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

    // 移除缓存项，返回被移除的节点；不存在则返回 nullptr
    NodePtr remove(const Key& key) {
        auto it = mainCache_.find(key);
        if (it == mainCache_.end()) return nullptr;

        NodePtr node = it->second;
        removeFromMain(node);
        mainCache_.erase(it);
        return node;
    }

    size_t size() const { return mainCache_.size(); }

    // 检查主缓存是否包含指定键
    bool contain(const Key& key) { return mainCache_.find(key) != mainCache_.end(); }

    // 设置主缓存目标容量（ARC 中 T1 的目标容量 p）
    // 缩容时按 LRU 顺序把溢出条目移入幽灵缓存，并返回这些条目
    std::vector<NodePtr> setCapacity(size_t capacity) {
        capacity_ = capacity;
        std::vector<NodePtr> evicted;
        while (mainCache_.size() > capacity_) {
            if (auto node = evictLeastRecent()) evicted.push_back(node);
        }
        return evicted;
    }

    // 设置幽灵缓存容量（对应 ARC 不变量 |T1| + |B1| <= 总容量）
    // 缩容时丢弃最旧的幽灵条目
    void setGhostCapacity(size_t ghostCapacity) {
        ghostCapacity_ = ghostCapacity;
        while (ghostCache_.size() > ghostCapacity_) removeOldestGhost();
    }

    // 幽灵缓存条目数，供 ARC 计算自适应增量使用
    size_t ghostSize() const { return ghostCache_.size(); }

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

    // 新增节点，返回为腾出空间而驱逐的节点；未发生驱逐或目标容量为 0 时返回 nullptr
    NodePtr addNewNode(const Key& key, const Value& value) {
        if (capacity_ == 0) return nullptr;  // 目标容量为 0：该部分暂不驻留条目

        NodePtr evicted = nullptr;
        if (mainCache_.size() >= capacity_) evicted = evictLeastRecent();

        auto node = std::make_shared<NodeType>(key, value);
        addToMain(node);
        mainCache_[key] = node;
        return evicted;
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
    // 返回被驱逐的节点；主缓存为空时返回 nullptr
    NodePtr evictLeastRecent() {
        auto oldest = mainTail_->prev_.lock();
        if (oldest == mainHead_ || oldest == nullptr) return nullptr;

        // 从主缓存链表移除
        removeFromMain(oldest);
        mainCache_.erase(oldest->getKey());

        if (ghostCache_.size() >= ghostCapacity_) removeOldestGhost();

        // 将节点添加到幽灵缓存链表
        addToGhost(oldest);
        return oldest;
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