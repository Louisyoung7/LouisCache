#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>

#include "ARC/ArcNode.h"

namespace louis::cache {
// 内部组件：不自行加锁，线程安全由调用方（ArcCache）的锁保证
template <typename Key, typename Value>
class ArcLfuPart {
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqListMap = std::map<size_t, std::list<NodePtr>>;

   public:
    ArcLfuPart(int capacity) : capacity_(capacity), ghostCapacity_(capacity), minFreq_(0) {
        initializeLists();
    }

    void put(Key key, Value value) {
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) {
            updateExistingNode(it->second, value);
        } else {
            addNewNode(key, value);
        }
    }

    bool get(const Key& key, Value& value) {
        auto it = mainCache_.find(key);

        if (it != mainCache_.end()) {
            value = it->second->getValue();
            updateNodeFreq(it->second);
            return true;
        }
        return false;
    }

    void remove(const Key& key) {
        auto it = mainCache_.find(key);
        if (it == mainCache_.end()) return;

        auto node = it->second;
        size_t freq = node->getAccessCount();

        // 节点存于对应频次的链表中，需先摘除，再维护频次映射与最小访问频次
        auto listIt = freqListMap_.find(freq);
        if (listIt != freqListMap_.end()) {
            listIt->second.remove(node);
            if (listIt->second.empty()) {
                freqListMap_.erase(listIt);
                if (freq == minFreq_) {
                    minFreq_ = freqListMap_.empty() ? 0 : freqListMap_.begin()->first;
                }
            }
        }

        mainCache_.erase(it);
    }

    size_t size() const { return mainCache_.size(); }

    // 检查主缓存是否包含指定键
    bool contain(const Key& key) { return mainCache_.find(key) != mainCache_.end(); }

    // 增加主缓存容量
    void increaseCapacity() { capacity_++; }

    // 减少主缓存容量
    bool decreaseCapacity() {
        if (capacity_ <= 0) return false;

        if (mainCache_.size() == capacity_) evictLeastFreq();

        --capacity_;
        return true;
    }

    // 检查幽灵缓存是否包含指定键，并删除该缓存项
    bool tryToRemoveGhost(const Key& key) {
        auto it = ghostCache_.find(key);

        if (it != ghostCache_.end()) {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

   private:
    // 初始化幽灵缓存链表
    void initializeLists() {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();

        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    // 更新已有节点的值
    void updateExistingNode(NodePtr node, const Value& value) {
        node->setValue(value);
        updateNodeFreq(node);
    }

    void addNewNode(const Key& key, const Value& value) {
        // 如果主缓存容量已满，先驱逐最旧的节点
        if (mainCache_.size() >= capacity_) evictLeastFreq();

        // 创建节点添加到链表中
        // 如果链表不存在要先创建
        auto node = std::make_shared<NodeType>(key, value);
        if (freqListMap_.find(1) == freqListMap_.end()) {
            freqListMap_[1] = std::list<NodePtr>();
        }
        freqListMap_[1].push_front(node);
        mainCache_[key] = node;

        // 更新最小访问频次
        minFreq_ = 1;
    }

    // 更新节点访问频次，同时维护频率链表映射
    void updateNodeFreq(NodePtr node) {
        // 更新节点频次
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount();
        size_t newFreq = node->getAccessCount();

        // 将节点从旧链表移除
        // 如果旧链表为空，将旧链表从频率链表映射移除，并更新最小访问频次
        auto& oldList = freqListMap_[oldFreq];
        oldList.remove(node);

        if (oldList.empty()) {
            freqListMap_.erase(oldFreq);
            if (oldFreq == minFreq_) {
                minFreq_ = newFreq;
            }
        }

        // 将节点添加到新链表中
        // 如果新链表不存在，要先创建
        if (freqListMap_.find(newFreq) == freqListMap_.end()) {
            freqListMap_[newFreq] = std::list<NodePtr>();
        }
        freqListMap_[newFreq].push_front(node);
    }

    // 从主缓存驱逐最少使用频次的节点，并添加到幽灵缓存链表
    void evictLeastFreq() {
        if (freqListMap_.empty()) return;

        // 移除最小访问频次链表的最后一个节点
        auto& leastList = freqListMap_[minFreq_];
        if (leastList.empty()) return;

        auto node = leastList.back();
        leastList.pop_back();
        mainCache_.erase(node->getKey());

        // 如果移除的节点恰好是最后一个节点，移除后链表为空，需要更新最小访问频次
        if (leastList.empty()) {
            freqListMap_.erase(minFreq_);
            if (!freqListMap_.empty()) {
                minFreq_ = freqListMap_.begin()->first;
            } else {
                minFreq_ = 0;
            }
        }

        // 如果幽灵缓存链表满了，移除幽灵缓存链表最旧的节点
        if (ghostCache_.size() >= ghostCapacity_) removeOldestGhost();

        // 添加到幽灵缓存链表
        addToGhost(node);
    }

    /// Ghost Cache 操作

    void addToGhost(NodePtr node) {
        if (ghostCache_.size() >= ghostCapacity_) removeOldestGhost();

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

    // 从幽灵缓存中移除最旧的缓存项
    void removeOldestGhost() {
        auto oldestNode = ghostTail_->prev_.lock();
        if (oldestNode == nullptr || oldestNode == ghostHead_) return;

        removeFromGhost(oldestNode);
        ghostCache_.erase(oldestNode->getKey());
    }

    size_t capacity_;
    NodeMap mainCache_;
    FreqListMap freqListMap_;  // LFU

    size_t ghostCapacity_;
    NodeMap ghostCache_;
    NodePtr ghostHead_;  // LRU
    NodePtr ghostTail_;

    size_t minFreq_;  // 最小访问频次，用于快速定位最小访问频次链表
};
}  // namespace louis::cache