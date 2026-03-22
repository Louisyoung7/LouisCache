#pragma once

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "LFU/FreqList.h"
#include "Policy.h"

namespace louis::cache {
template <typename Key, typename Value>
class LfuAgingCache : public Policy<Key, Value> {
    using Node = typename FreqList<Key, Value>::LfuNode;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqToFreqListMap = std::unordered_map<int, std::unique_ptr<FreqList<Key, Value>>>;

    int capacity_;      // 总缓存容量
    int minFreq_;       // 最小访问频次，用于快速查找最小访问频次链表
    int maxAvgFreq_;    // 最大平均访问频次，用于避免访问频次溢出
    int curAvgFreq_;    // 当前平均访问频次
    int curTotalFreq_;  // 当前总访问频次，用于计算当前平均访问频次
    std::mutex mutex_;
    NodeMap nodeMap_;
    FreqToFreqListMap freqToFreqListMap_;  // 访问频次 ： 访问频次链表

   public:
    LfuAgingCache(int capacity, int maxAvgFreq = 100000)
        : capacity_(capacity),
          minFreq_(std::numeric_limits<int>::max()),
          maxAvgFreq_(maxAvgFreq),
          curAvgFreq_(0),
          curTotalFreq_(0) {
    }
    void put(Key key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);

        if (it != nodeMap_.end()) {
            // 重置value值
            it->second->value = value;
            // 更新缓存项
            getInternal(it->second, value);
        } else {
            putInternal(key, value);
        }
    }

    bool get(const Key& key, Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);

        if (it != nodeMap_.end()) {
            getInternal(it->second, value);
            return true;
        }

        return false;
    }

    Value get(const Key& key) {
        Value value{};
        get(key, value);
        return value;
    }

   private:
    // 只负责添加缓存项和更新各项访问频次
    void putInternal(Key key, Value value) {
        // 如果缓存已满，删除最不经常使用的节点
        if (nodeMap_.size() >= capacity_) {
            kickOut();
        }

        // 创建节点并添加到缓存中
        NodePtr node = std::make_shared<Node>(key, value);
        addToList(node);
        nodeMap_[key] = node;

        // 更新各项访问频次
        increaseFreq();
        minFreq_ = std::min(minFreq_, 1);
    }

    // 只负责查询缓存项和更新各项访问频次
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
        if (node->freq - 1 == minFreq_ && freqToFreqListMap_[minFreq_]->isEmpty()) {
            minFreq_++;
        }

        // 更新其余的访问频次项
        increaseFreq();
    }

    // 移除最不经常使用的节点
    void kickOut() {
        auto node = freqToFreqListMap_[minFreq_]->getLastNode();
        if (node) {
            removeFromList(node);
            nodeMap_.erase(node->key);
            decreaseFreq(node->freq);
        }
    }

    void addToList(NodePtr node) {
        int freq = node->freq;

        auto it = freqToFreqListMap_.find(freq);

        if (it == freqToFreqListMap_.end()) {
            // 如果没有相应链表则创建
            freqToFreqListMap_.insert({freq,std::make_unique<FreqList<Key, Value>>(freq)});
        }

        freqToFreqListMap_[freq]->addNode(node);
    }

    void removeFromList(NodePtr node) {
        int freq = node->freq;

        auto it = freqToFreqListMap_.find(freq);

        if (it == freqToFreqListMap_.end()) {
            return;
        } else {
            freqToFreqListMap_[freq]->removeNode(node);
        }
    }

    // 增加访问次数
    void increaseFreq() {
        curTotalFreq_++;
        if (nodeMap_.empty()) {
            curAvgFreq_ = 0;
        } else {
            curAvgFreq_ = curTotalFreq_ / nodeMap_.size();
        }

        // 如果当前平均访问次数超出阈值，需要处理
        if (curAvgFreq_ >= maxAvgFreq_) {
            handleOverMaxAvgFreq();
        }
    }

    // 减少访问次数
    void decreaseFreq(int n) {
        curTotalFreq_ -= n;
        if (nodeMap_.empty()) {
            curAvgFreq_ = 0;
        } else {
            curAvgFreq_ = curTotalFreq_ / nodeMap_.size();
        }
    }

    // 处理当前平均访问次数超出阈值的情况
    // 将所有节点的访问频次都减去 maxAvgFreq / 2
    void handleOverMaxAvgFreq() {
        // 确定衰减因子
        int decay = maxAvgFreq_ / 2;

        // 遍历节点映射
        for (auto it = nodeMap_.begin(); it != nodeMap_.end(); ++it) {
            if (!it->second) {
                continue;
            }

            // 获取节点
            auto node = it->second;

            // 先将节点移除
            removeFromList(node);

            // 保留原先的访问频次
            int oldFreq = node->freq;

            // 访问频次减去衰减因子
            node->freq -= decay;

            if (node->freq <= 0) {
                node->freq = 1;
            }

            // 获取访问频次的变化量
            int delta = node->freq - oldFreq;

            // 更新总访问频次
            curTotalFreq_ += delta;

            // 再将节点重新加入
            addToList(node);
        }
    }

    // 更新最小访问频次
    void updateMinFreq() {
        minFreq_ = std::numeric_limits<int>::max();
        // 遍历频率链表映射，从中选出最小访问频次
        for (const auto& pair : freqToFreqListMap_) {
            if (pair.second && !pair.second->isEmpty()) {
                minFreq_ = std::min(minFreq_, pair.first);
            }
        }

        // 如果映射为空，将最小访问频次初始化为1
        if (minFreq_ == std::numeric_limits<int>::max()) {
            minFreq_ = 1;
        }
    }
};
}  // namespace louis::cache