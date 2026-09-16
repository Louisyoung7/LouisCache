#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "LFU/FreqList.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value>
class LfuCache : public Policy<Key, Value> {
    using Node = typename FreqList<Key, Value>::LfuNode;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqToFreqListMap = std::unordered_map<size_t, std::unique_ptr<FreqList<Key, Value>>>;

   public:
    explicit LfuCache(size_t capacity) : capacity_(capacity), minFreq_(1) {}

    // 如果缓存项存在，更新并提升其访问频次
    // 如果缓存项不存在，添加新节点（缓存已满时先淘汰最不经常使用的节点）
    void put(Key key, Value value) override {
        if (capacity_ == 0) return;  // 容量 0：无处可存，直接丢弃（条目未曾驻留，不通知）
        std::optional<std::pair<Key, Value>> evicted;  // 锁内收集，锁外分发
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = nodeMap_.find(key);

            if (it != nodeMap_.end()) {
                // 重置value值
                it->second->value = value;
                // 更新缓存项
                getInternal(it->second, value);
            } else {
                evicted = putInternal(key, value);
            }
        }
        if (evicted) {
            // notifyLeave 位于依赖基类 Policy<Key, Value> 中，需 this-> 才能在实例化时找到
            this->notifyLeave(evicted->first, evicted->second, LeaveReason::Evicted);
        }
    }

    // 尝试获取缓存项，如果存在则提升其访问频次
    bool get(const Key& key, Value& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);

        if (it != nodeMap_.end()) {
            getInternal(it->second, value);
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
            removeFromList(it->second);
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

   private:
    // 只负责添加缓存项，缓存已满时先淘汰，返回被淘汰的键值对
    std::optional<std::pair<Key, Value>> putInternal(Key key, Value value) {
        std::optional<std::pair<Key, Value>> evicted;

        // 如果缓存已满，删除最不经常使用的节点
        if (nodeMap_.size() >= capacity_) {
            evicted = kickOut();
        }

        // 创建节点并添加到缓存中
        NodePtr node = std::make_shared<Node>(key, value);
        addToList(node);
        nodeMap_[key] = node;

        // 新节点访问频次为1，最小访问频次必然是1
        minFreq_ = 1;
        return evicted;
    }

    // 只负责查询缓存项并提升其访问频次
    void getInternal(NodePtr node, Value& value) {
        // 获取值
        value = node->value;

        // 从原频率链表删除
        removeFromList(node);

        // 更新节点访问频次
        node->freq++;

        // 再重新添加到链表
        addToList(node);

        // 如果原先存在的链表因为节点移动变成了空链表，而原先链表恰好是最小访问频次链表
        // 此时需要更新最小访问频次
        auto oldFreq = node->freq - 1;
        if (oldFreq == minFreq_) {
            auto it = freqToFreqListMap_.find(oldFreq);
            if (it != freqToFreqListMap_.end() && it->second->isEmpty()) {
                minFreq_++;
            }
        }
    }

    // 移除最不经常使用的节点
    // 调用方需持有锁；只做数据结构变更，不触发回调，由上层在锁外分发
    std::optional<std::pair<Key, Value>> kickOut() {
        auto it = freqToFreqListMap_.find(minFreq_);
        if (it == freqToFreqListMap_.end()) {
            return std::nullopt;
        }

        auto node = it->second->getLastNode();
        if (!node) {
            return std::nullopt;
        }

        removeFromList(node);
        nodeMap_.erase(node->key);

        return std::make_pair(std::move(node->key), std::move(node->value));
    }

    void addToList(NodePtr node) {
        size_t freq = node->freq;

        auto it = freqToFreqListMap_.find(freq);

        if (it == freqToFreqListMap_.end()) {
            // 如果没有相应链表则创建
            freqToFreqListMap_.insert({freq, std::make_unique<FreqList<Key, Value>>(freq)});
        }

        freqToFreqListMap_[freq]->addNode(node);
    }

    void removeFromList(NodePtr node) {
        size_t freq = node->freq;

        auto it = freqToFreqListMap_.find(freq);

        if (it == freqToFreqListMap_.end()) {
            return;
        } else {
            freqToFreqListMap_[freq]->removeNode(node);
        }
    }

    size_t capacity_;           // 总缓存容量
    size_t minFreq_;            // 最小访问频次，用于快速查找最小访问频次链表
    mutable std::mutex mutex_;  // size() 为 const，需 mutable 才能加锁
    NodeMap nodeMap_;
    FreqToFreqListMap freqToFreqListMap_;  // 访问频次 ： 访问频次链表
};
}  // namespace louis::cache
