#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "ARC/ArcNode.h"

namespace louis::cache {
template <typename Key, typename Value>
class ArcLfuPart {
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqListMap = std::map<size_t, std::list<NodePtr>>;

    size_t capacity_;
    size_t ghostCapacity_;
    size_t minFreq_;  // 最小访问频次，用于快速定位最小访问频次链表
    std::mutex mutex_;

    NodeMap mainCache_;        // 主缓存映射
    NodeMap ghostCache_;       // 幽灵缓存映射，存储从主缓存移除的缓存项
    FreqListMap freqListMap_;  // 频率链表映射，快速访问不同频次的链表
    NodePtr ghostHead_;
    NodePtr ghostTail_;

   public:
    ArcLfuPart(int capacity) : capacity_(capacity), ghostCapacity_(capacity), minFreq_(0) {
        initializeLists();
    }

    bool put(Key key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);

        if (it != mainCache_.end()) {
            updateExistingNode(it->second, value);
            return true;
        } else {
            addNewNode(key, value);
            return true;
        }

        return false;
    }

    bool get(const Key& key, Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);

        if (it != mainCache_.end()) {
            value = it->second->getValue();
            updateNodeFreq(it->second);
            return true;
        }

        return false;
    }

    // 检查主缓存释放包含指定键
    bool contain(const Key& key) {
        return mainCache_.find(key) != mainCache_.end();
    }

    // 检查幽灵缓存是否包含指定键，并删除该缓存项
    bool checkGhost(const Key& key) {
        auto it = ghostCache_.find(key);

        if (it != ghostCache_.end()) {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }

        return false;
    }

    // 增加主缓存容量
    void increaseCapacity() {
        capacity_++;
    }

    // 减少主缓存容量
    bool decreaseCapacity() {
        if (capacity_ <= 0) {
            return false;
        }

        if (mainCache_.size() == capacity_) {
            evictLeastFreq();
        }

        capacity_--;
        return true;
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
    bool updateExistingNode(NodePtr node, const Value& value) {
        node->setValue(value);
        updateNodeFreq(node);
        return true;
    }

    // 添加到头部
    bool addNewNode(const Key& key, const Value& value) {
        // 如果主缓存容量已满，先驱逐最旧的节点
        if (mainCache_.size() >= capacity_) {
            evictLeastFreq();
        }

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

        return true;
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
        if (freqListMap_.empty()) {
            return;
        }

        // 移除最小访问频次链表的最后一个节点
        auto& leastList = freqListMap_[minFreq_];
        if (leastList.empty()) {
            return;
        }
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
        if (ghostCache_.size() >= ghostCapacity_) {
            removeOldestGhost();
        }

        // 添加到幽灵缓存链表
        addToGhost(node);
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

    // 添加到头部
    void addToGhost(NodePtr node) {
        if (ghostCache_.size() >= ghostCapacity_) {
            removeOldestGhost();
        }

        auto prev = ghostHead_;
        auto next = ghostHead_->next_;

        prev->next_ = node;
        node->prev_ = prev;

        node->next_ = next;
        next->prev_ = node;

        ghostCache_[node->getKey()] = node;
    }

    // 从幽灵缓存中移除最旧的缓存项
    void removeOldestGhost() {
        auto oldestNode = ghostTail_->prev_.lock();
        if (oldestNode == nullptr || oldestNode == ghostHead_) {
            return;
        }

        removeFromGhost(oldestNode);
        ghostCache_.erase(oldestNode->getKey());
    }
};
}  // namespace louis::cache