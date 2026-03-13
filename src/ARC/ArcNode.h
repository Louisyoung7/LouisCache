#pragma once

#include <cstddef>
#include <memory>

namespace louis::cache {
template <typename Key, typename Value>
class ArcNode {
    Key key_;
    Value value_;
    size_t accessCount_;  // 节点访问次数
    std::weak_ptr<ArcNode> prev_;
    std::shared_ptr<ArcNode> next_;

   public:
    ArcNode() : accessCount_(1), next_(nullptr) {
    }
    ArcNode(Key key, Value value) : key_(key), value_(value), accessCount_(1), next_(nullptr) {
    }

    // Getter

    const Key& getKey() const {
        return key_;
    }

    const Value& getValue() const {
        return value_;
    }

    const size_t getAccessCount() const {
        return accessCount_;
    }

    // Setter

    void setValue(const Value& value) {
        value_ = value;
    }

    void incrementAccessCount() {
        accessCount_++;
    }

    void resetAccessCount() {
        accessCount_ = 1;
    }

    template <typename K, typename V>
    friend class ArcLruPart;

    template <typename K, typename V>
    friend class ArcLfuPart;
};
}  // namespace louis::cache