#pragma once

#include <memory>

namespace louis::cache {
template <typename Key, typename Value>
class LruCache;

template <typename Key, typename Value>
class LruNode {
    Key key_;
    Value value_;
    std::weak_ptr<LruNode<Key, Value>> prev_;
    std::shared_ptr<LruNode<Key, Value>> next_;

   public:
    LruNode(Key key, Value value) : key_(key), value_(value) {
    }

    // Getter
    const Key& getKey() const {
        return key_;
    }

    const Value& getValue() const {
        return value_;
    }

    // Setter
    void setValue(const Value& value) {
        value_ = value;
    }

    friend class LruCache<Key, Value>;
};
}  // namespace louis::cache