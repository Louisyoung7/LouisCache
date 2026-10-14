#pragma once
#include <string>
#include <vector>

class Embedder {
   public:
    virtual ~Embedder() = default;
    virtual std::vector<float> embed(const std::string& text) const = 0;
    virtual size_t dim() const = 0;
    // 模型指纹：向量空间标识，持久化索引加载时校验，不同指纹不可比较
    virtual std::string modelId() const = 0;
};

class HashEmbedder : public Embedder {
   public:
    HashEmbedder() = default;
    ~HashEmbedder() = default;
};