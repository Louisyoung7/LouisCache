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
    // 空构造：容量（主缓存与幽灵缓存）不在此处给定，
    // 由 ArcCache::applyPartitions 按自适应参数 p 统一设置
    ArcLfuPart() : capacity_(0), ghostCapacity_(0), minFreq_(0) { initializeLists(); }

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

    // 设置主缓存目标容量（ARC 中 T2 的目标容量，即 总容量 - p）
    // 缩容时按频次由低到高把溢出条目移入幽灵缓存
    void setCapacity(size_t capacity) {
        capacity_ = capacity;
        while (mainCache_.size() > capacity_) evictLeastFreq();
    }

    // 设置幽灵缓存容量（对应 ARC 不变量 |T2| + |B2| <= 总容量）
    // 缩容时丢弃最旧的幽灵条目
    void setGhostCapacity(size_t ghostCapacity) {
        ghostCapacity_ = ghostCapacity;
        while (ghostCache_.size() > ghostCapacity_) removeOldestGhost();
    }

    // 幽灵缓存条目数，供 ARC 计算自适应增量使用
    size_t ghostSize() const { return ghostCache_.size(); }

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
        if (capacity_ == 0) return;  // 目标容量为 0：该部分暂不驻留条目

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
        // 用 find 而非 operator[]，避免为不存在的频次插入空链表而污染 freqListMap_
        auto listIt = freqListMap_.find(minFreq_);
        if (listIt == freqListMap_.end() || listIt->second.empty()) return;

        // 移除最小访问频次链表的最后一个节点
        auto node = listIt->second.back();
        listIt->second.pop_back();
        mainCache_.erase(node->getKey());

        // 如果移除的节点恰好是最后一个节点，移除后链表为空，需要更新最小访问频次
        if (listIt->second.empty()) {
            freqListMap_.erase(listIt);
            if (!freqListMap_.empty()) {
                minFreq_ = freqListMap_.begin()->first;
            } else {
                minFreq_ = 0;
            }
        }

        // 添加到幽灵缓存链表（addToGhost 内部维护幽灵缓存容量上限）
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