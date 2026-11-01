#pragma once
#include <stdint.h>

namespace louis::cache {

struct CacheStats {
    uint64_t exactHits = 0;     // 精确命中次数
    uint64_t semanticHits = 0;  // 语义命中次数
    uint64_t misses = 0;        // 未命中次数
    uint64_t evictions = 0;     // LeaveReason::Evicted
    uint64_t expirations = 0;   // LeaveReason::Expired
    uint64_t puts = 0;
};

}  // namespace louis::cache