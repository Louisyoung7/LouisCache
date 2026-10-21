#pragma once
#include <cstddef>

namespace louis::cache {

struct Options {
    size_t capacity = 1000;         // EntryStore 容量
    float hitThreshold = 0.85f;     // 语义命中阈值
    size_t searchK = 5;             // ANN 搜索 K 个最近邻
    bool exactIndexEnabled = true;  // 是否启用精确索引搜索
};

}  // namespace louis::cache