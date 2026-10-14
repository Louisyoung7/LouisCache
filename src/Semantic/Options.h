#pragma once
#include <cstddef>

struct Options {
    float hitThreshold = 0.85f;     // 语义命中阈值
    size_t searchK = 5;             // ANN 搜索 K 个最近邻
    bool exactIndexEnabled = true;  // 是否启用精确索引搜索
};