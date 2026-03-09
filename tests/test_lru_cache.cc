#include <iostream>
#include "LRU/LruCache.h"

using namespace louis::cache;

int main() {
    // 创建一个容量为 2 的 LRU 缓存
    LruCache<int, std::string> cache(2);
    
    // 添加缓存项
    cache.put(1, "one");
    cache.put(2, "two");
    
    // 查询缓存项
    std::cout << "Get 1: " << cache.get(1) << std::endl;
    
    // 添加新缓存项，会淘汰最久未使用的 2
    cache.put(3, "three");
    
    // 验证 2 是否被淘汰
    std::string value;
    if (cache.get(2, value)) {
        std::cout << "Get 2: " << value << std::endl;
    } else {
        std::cout << "Get 2: not found" << std::endl;
    }
    
    // 验证 3 是否存在
    std::cout << "Get 3: " << cache.get(3) << std::endl;
    
    return 0;
}