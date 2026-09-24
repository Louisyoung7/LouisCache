# LouisCache

LouisCache 是一个高性能的 C++ 缓存库，实现了多种缓存替换策略，包括 LRU、LFU 和 ARC。

## 特性

- **多种缓存策略**：
  - LRU (Least Recently Used) - 最近最少使用
  - LFU (Least Frequently Used) - 最不经常使用
  - ARC (Adaptive Replacement Cache) - 自适应替换缓存
- **缓存项离开通知**（LeaveCallback）：缓存项因淘汰、显式移除或过期离开时回调通知使用方，可同步清理外部资源
- **线程安全**：支持多线程环境
- **高性能**：优化的实现，减少锁竞争
- **模板化设计**：支持任意类型的键值对
- **内存管理**：自动管理缓存大小，防止内存溢出
- **纯头文件实现**：无编译依赖，直接包含使用

## 支持的缓存实现

### LRU 系列（已适配统一接口）
- `LruCache` - 基础 LRU 缓存
- `LruKCache` - K 最近最少使用缓存（访问 k 次后晋升进主缓存）
- `ShardedLruCache` - 分片 LRU 缓存（提高并发性能）

### LFU 系列（已适配统一接口）
- `LfuCache` - 基础 LFU 缓存（频次最低者优先淘汰，同频次按插入顺序）
- `ShardedLfuCache` - 分片 LFU 缓存（提高并发性能）

### ARC 系列（已适配统一接口）
- `ArcCache` - 自适应替换缓存：T1（LRU 部分）与 T2（LFU 部分）共享总容量，由自适应参数 p 动态划分；命中幽灵缓存（B1/B2）时按两侧幽灵缓存大小的比例调整 p，自动在"近期性"与"频次"之间倾斜

## 项目结构

```
LouisCache/
├── docs/               # 设计文档
├── src/                # 头文件目录
│   ├── ARC/            # ARC 缓存实现
│   ├── LFU/            # LFU 缓存实现
│   ├── LRU/            # LRU 缓存实现
│   └── Policy.h        # 缓存策略抽象接口
├── tests/              # 单元测试（Google Test）
├── conanfile.py        # Conan 依赖配置
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

单元测试基于 Google Test（由 Conan 提供依赖）：

```bash
# 克隆仓库
git clone https://github.com/Louisyoung7/LouisCache.git
cd LouisCache

# 安装依赖（首次）
conan install . --output-folder=build --build=missing

# 配置并构建
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TEST=ON
cmake --build build

# 运行测试
ctest --test-dir build
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
// 使用 LRU-K 缓存（主缓存容量 100，历史队列容量 500）
#include "LRU/LruKCache.h"
louis::cache::LruKCache<int, std::string> lrukCache(100, 500);

// 使用分片 LRU 缓存（提高并发性能）
#include "LRU/ShardedLruCache.h"
louis::cache::ShardedLruCache<int, std::string> shardedCache(100);

// 使用 LFU 缓存
#include "LFU/LfuCache.h"
louis::cache::LfuCache<int, std::string> lfuCache(100);

// 使用分片 LFU 缓存（提高并发性能）
#include "LFU/ShardedLfuCache.h"
louis::cache::ShardedLfuCache<int, std::string> shardedLfuCache(100);

// 使用 ARC 缓存（第二个参数为转换阈值，条目访问达到阈值后从 T1 晋升到 T2，默认 2）
#include "ARC/ArcCache.h"
louis::cache::ArcCache<int, std::string> arcCache(100);
```

### 缓存项离开通知（LeaveCallback）

缓存项离开缓存时（容量淘汰 `Evicted`、显式移除 `Explicit`、过期 `Expired`），通过回调携带 key、value 和原因通知使用方，适用于同步清理外部资源（如向量索引、文件句柄）的场景：

```cpp
louis::cache::LruCache<int, std::string> cache(2);

cache.setLeaveCallback([](const int& key, const std::string& value,
                          louis::cache::LeaveReason reason) {
    switch (reason) {
        case louis::cache::LeaveReason::Evicted:
            // 容量淘汰：清理与该条目关联的外部资源
            break;
        case louis::cache::LeaveReason::Explicit:
            // remove() 显式移除
            break;
        case louis::cache::LeaveReason::Expired:
            // 过期失效
            break;
    }
});

cache.put(1, "value1");
cache.put(2, "value2");
cache.put(3, "value3");  // 1 被淘汰，触发回调
```

回调的重要约定：

- **锁外分发**：回调在缓存内部锁全部释放后触发，回调内可安全重入缓存任意方法（如再调 `get`/`put`/`size`），不会死锁，且能观察到离开后的最终状态
- **变更完成后触发**：回调触发时条目已从缓存移除，`size()` 等查询反映最终状态
- **内部变动不通知**：复合结构中，条目在内部各部分间的流转对外不可见，不触发回调；只有条目真正"离开缓存"时才通知。例如 `LruKCache` 中历史队列的晋升与淘汰、`ArcCache` 中条目从 T1（LRU 部分）到 T2（LFU 部分）的迁移、以及幽灵缓存（B1/B2）自身的挤出与命中均静默
- **ARC 特有语义**：`ArcCache` 中 T1、T2 任一主缓存部分发生容量驱逐均通知 `Evicted`；由于命中幽灵缓存会触发容量重划分、可能挤出对侧主缓存条目，因此 `get` 未命中也可能产生 `Evicted` 事件

## 测试

单元测试基于 Google Test，覆盖各策略的基础行为、淘汰语义、LeaveCallback 触发与静默场景、回调重入安全及多线程并发，当前 175 个用例全部通过（见上文构建步骤）。

## 缓存策略选择指南

- **LRU**：适用于访问模式具有时间局部性的场景
- **LFU**：适用于访问模式具有频率局部性的场景
- **ARC**：自动适应访问模式，适用于大多数场景

## 线程安全性

- 各实现内部以锁保证多线程访问安全；复合实现（如 `LruKCache`）以单锁串行化公共方法，保证复合操作原子
- 分片实现（`ShardedLruCache`、`ShardedLfuCache`）以独立锁降低锁竞争，提高并发吞吐；分片容量精确分摊，各分片容量之和恰为总容量
- 分片实现的 `size()` 为各分片分别加锁后求和，多线程下不是原子快照（分片结构的固有属性）
- 分片实现的拷贝与移动被禁用：分片回调捕获宿主对象指针，移动会导致悬垂
- 回调分发不持有缓存内部锁，回调内重入缓存安全（见 LeaveCallback 约定）

## 许可证

本项目采用 MIT 许可证。

## 贡献

欢迎提交 issue 和 pull request！

## 联系方式

如有问题或建议，请通过 GitHub Issues 与我联系。