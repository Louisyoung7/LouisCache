#pragma once

namespace louis::cache {
template <typename Key, typename Value>
class Policy {
   public:
    // 虚析构
    virtual ~Policy() = default;

    // 添加缓存项
    virtual void put(Key key, Value value) = 0;

    // 查询缓存项
    // value是传出参数
    virtual bool get(const Key& key, Value& value) = 0;

    // 查询缓存项，返回值
    virtual Value get(const Key& key) = 0;
};
}  // namespace louis::cache