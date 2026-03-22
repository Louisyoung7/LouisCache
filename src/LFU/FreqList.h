#pragma once

#include <memory>

namespace louis::cache {
template <typename Key, typename Value>
class FreqList {
    struct LfuNode {
        int freq;  // 节点访问频次
        Key key;
        Value value;
        std::weak_ptr<LfuNode> prev;
        std::shared_ptr<LfuNode> next;

        LfuNode() : freq(1), next(nullptr) {
        }
        LfuNode(Key key, Value value) : freq(1), key(key), value(value), next(nullptr) {
        }
    };

    using NodePtr = std::shared_ptr<LfuNode>;

    int freq_;  // 频率链表的访问频次（单个链表的每一个节点的访问频次相同）
    NodePtr dummyHead_;
    NodePtr dummyTail_;

    template<typename K, typename V>
    friend class LfuAgingCache;

   public:
    explicit FreqList(int n)
        : freq_(n), dummyHead_(std::make_shared<LfuNode>()), dummyTail_(std::make_shared<LfuNode>()) {
        dummyHead_->next = dummyTail_;
        dummyTail_->prev = dummyHead_;
    }

    void addNode(NodePtr node) {
        if (node && dummyHead_ && dummyTail_) {
            auto prev = dummyHead_;
            auto next = dummyHead_->next;

            prev->next = node;
            node->prev = prev;

            node->next = next;
            next->prev = node;
        }
    }

    void removeNode(NodePtr node) {
        if (node && dummyHead_ && dummyTail_) {
            if (node->prev.expired() || !node->next) {
                return;
            }

            auto prev = node->prev.lock();
            auto next = node->next;

            prev->next = next;
            next->prev = prev;

            node->prev.reset();
            node->next = nullptr;
        }
    }

    // 不包括尾节点
    // 用于删除最不经常访问的节点
    NodePtr getLastNode() const {
        auto lastNode = dummyTail_->prev.lock();

        if (lastNode == dummyHead_) {
            return nullptr;
        }

        return lastNode;
    }

    bool isEmpty() const {
        return dummyHead_->next == dummyTail_;
    }
};
}  // namespace louis::cache