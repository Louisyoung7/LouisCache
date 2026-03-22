# LouisCache

LouisCache 是一个高性能的 C++ 缓存库，实现了多种缓存替换策略，包括 LRU、LFU 和 ARC。

## 特性

- **多种缓存策略**：
  - LRU (Least Recently Used) - 最近最少使用
  - LFU (Least Frequently Used) - 最不经常使用
  - ARC (Adaptive Replacement Cache) - 自适应替换缓存
- **线程安全**：支持多线程环境
- **高性能**：优化的实现，减少锁竞争
- **模板化设计**：支持任意类型的键值对
- **内存管理**：自动管理缓存大小，防止内存溢出
- **纯头文件实现**：无编译依赖，直接包含使用

## 支持的缓存实现

### LRU 系列
- `LruCache` - 基础 LRU 缓存
- `LruKCache` - K 最近最少使用缓存
- `ShardedLruCache` - 分片 LRU 缓存（提高并发性能）

### LFU 系列
- `LfuAgingCache` - 带老化机制的 LFU 缓存
- `ShardedLfuCache` - 分片 LFU 缓存

### ARC 系列
- `ArcCache` - 自适应替换缓存（结合 LRU 和 LFU 的优点）

## 项目结构

```
LouisCache/
├── src/                # 头文件目录
│   ├── ARC/            # ARC 缓存实现
│   ├── LFU/            # LFU 缓存实现
│   ├── LRU/            # LRU 缓存实现
│   └── Policy.h        # 缓存策略抽象接口
├── tests/              # 测试目录
│   ├── benchmark.cc    # 性能基准测试（唯一的源文件）
│   └── hitTest.png     # 命中率测试结果
├── CMakeLists.txt      # CMake 构建文件
└── README.md           # 项目说明
```

## 构建与使用

### 依赖
- C++17 或更高版本
- CMake 3.10 或更高版本（仅用于构建测试）

### 作为纯头文件库使用

由于 LouisCache 是纯头文件实现，您可以直接将 `src` 目录复制到您的项目中，并在需要使用的地方包含相应的头文件：

```cpp
// 包含所需的缓存头文件
#include "LRU/LruCache.h"  // 或其他缓存实现

// 使用缓存
louis::cache::LruCache<int, std::string> cache(100);
```

### 构建测试

如果您想运行性能测试，可以按照以下步骤构建：

```bash
# 克隆仓库
git clone https://github.com/Louisyoung7/LouisCache.git
cd LouisCache

# 创建构建目录
mkdir build && cd build

# 配置 CMake
cmake -DBUILD_TEST=ON ..

# 构建测试
make

# 运行测试
cd bin && ./benchmark
```

## 使用示例

### 基本用法

```cpp
#include "LRU/LruCache.h"

int main() {
    // 创建一个容量为 100 的 LRU 缓存
    louis::cache::LruCache<int, std::string> cache(100);
    
    // 添加缓存项
    cache.put(1, "value1");
    cache.put(2, "value2");
    
    // 获取缓存项
    std::string value;
    if (cache.get(1, value)) {
        std::cout << "Found value: " << value << std::endl;
    }
    
    // 或者直接获取
    std::string value2 = cache.get(2);
    std::cout << "Value: " << value2 << std::endl;
    
    return 0;
}
```

### 使用其他缓存策略

```cpp
// 使用 LFU 缓存
#include "LFU/LfuAgingCache.h"
louis::cache::LfuAgingCache<int, std::string> lfuCache(100);

// 使用 ARC 缓存
#include "ARC/ArcCache.h"
louis::cache::ArcCache<int, std::string> arcCache(100);
```

## 性能测试

项目包含一个性能基准测试程序（`tests/benchmark.cc`），用于比较不同缓存策略的性能和命中率。这是项目中唯一的源文件。

### 运行测试

```bash
cd build/bin
./benchmark
```

## 缓存策略选择指南

- **LRU**：适用于访问模式具有时间局部性的场景
- **LFU**：适用于访问模式具有频率局部性的场景
- **ARC**：自动适应访问模式，适用于大多数场景

## 线程安全性

- 分片缓存实现（`ShardedLruCache` 和 `ShardedLfuCache`）提供更好的并发性能
- 基础缓存实现也支持多线程访问

## 许可证

本项目采用 MIT 许可证。

## 贡献

欢迎提交 issue 和 pull request！

## 联系方式

如有问题或建议，请通过 GitHub Issues 与我联系。