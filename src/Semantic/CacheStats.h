#pragma once
#include <cstddef>

namespace louis::cache {

struct CacheStats {
    size_t exactHits = 0;     // 精确命中次数
    size_t semanticHits = 0;  // 语义命中次数
    size_t misses = 0;        // 未命中次数
    size_t evictions = 0;     // LeaveReason::Evicted
    size_t expiration = 0;    // LeaveReason::Expired
    size_t puts = 0;
};

}  // namespace louis::cache