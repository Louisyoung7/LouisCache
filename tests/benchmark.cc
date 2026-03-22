#include <array>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "ARC/ArcCache.h"
#include "LFU/LfuAgingCache.h"
#include "LRU/LruCache.h"
#include "LRU/LruKCache.h"
#include "Policy.h"

using namespace louis::cache;

// 辅助函数，打印结果
void printResults(const std::string& testName, int capacity, const std::vector<int>& get_operations,
                  const std::vector<int>& hits) {
    std::cout << "=== " << testName << " 结果汇总 ===" << std::endl;
    std::cout << "缓存大小: " << capacity << std::endl;

    std::vector<std::string> names;
    if (hits.size() == 3) {
        names = {"LRU", "LFU", "ARC"};
    } else if (hits.size() == 4) {
        names = {"LRU", "LFU", "ARC", "LRU-K"};
    } else if (hits.size() == 5) {
        names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};
    }

    for (size_t i = 0; i < hits.size(); ++i) {
        // 计算命中率
        double hitRate = 100.0 * hits[i] / get_operations[i];
        std::cout << (i < names.size() ? names[i] : "Algorithm " + std::to_string(i + 1)) << " - 命中率: " << std::fixed
                  << std::setprecision(2) << hitRate << "% ";

        // 添加具体命中次数和总操作次数
        std::cout << "(" << hits[i] << "/" << get_operations[i] << ")" << std::endl;
    }

    std::cout << std::endl;
}

void testHotDataAccess() {
    std::cout << "\n=== 测试场景1：热点数据访问测试 ===" << std::endl;

    const int CAPACITY = 20;        // 缓存容量
    const int OPERATIONS = 500000;  // 总操作次数
    const int HOT_KEYS = 20;        // 热点数据数量
    const int COLD_KEYS = 5000;     // 冷数据数量

    // 创建真随机数引擎
    std::random_device rd;
    // 初始化伪随机数引擎
    std::mt19937 gen(rd());

    // 预热分布：从冷数据中随机选择key
    std::uniform_int_distribution<int> preheatDist(0, HOT_KEYS + COLD_KEYS - 1);

    // 读写操作分布：30%概率写入，70%概率读取
    std::bernoulli_distribution writeDist(0.3);

    // 冷热数据访问分布：70%概率访问热点数据，30%概率访问冷数据
    std::bernoulli_distribution hotDataDist(0.7);

    // 范围分布：热点数据20个，[0, 19]，冷数据5000个，[20, 5019]
    std::uniform_int_distribution<int> hotKeyDist(0, HOT_KEYS - 1);
    std::uniform_int_distribution<int> coldKeyDist(0, COLD_KEYS - 1);

    // 创建缓存对象
    LruCache<int, std::string> lru(CAPACITY);
    // 默认最大访问次数是100000， 这里不可能达到这个数，相当于普通LFU
    LfuAgingCache<int, std::string> lfu(CAPACITY);
    ArcCache<int, std::string> arc(CAPACITY);
    // 访问历史队列的大小设置为所有数据的总数
    LruKCache<int, std::string> lruk(CAPACITY, HOT_KEYS + COLD_KEYS);
    LfuAgingCache<int, std::string> lfuAging(CAPACITY, 20000);

    // 指针数组，基类指针指向派生类
    std::array<Policy<int, std::string>*, 5> caches{&lru, &lfu, &arc, &lruk, &lfuAging};

    // 记录命中次数和总操作次数
    std::vector<int> hits(5, 0);
    std::vector<int> getOperations(5, 0);
    std::vector<std::string> names{"LRU", "LFU", "ARC", "LRU-k", "LFU-Aging"};

    // 为所有的缓存对象添加相同的操作序列测试
    for (size_t cacheIdx = 0; cacheIdx < caches.size(); ++cacheIdx) {
        // 先预热缓存，随机选择 CAPACITY 个key
        std::set<int> preheatedKeys;
        while (preheatedKeys.size() < CAPACITY) {
            // 获取key
            int key = preheatDist(gen);

            if (preheatedKeys.find(key) == preheatedKeys.end()) {
                preheatedKeys.insert(key);
                caches[cacheIdx]->put(key, "preheat_value" + std::to_string(key));
            }
        }

        // 交替进行put和get操作，模拟真实场景
        for (int op = 0; op < OPERATIONS; ++op) {
            // 获取要操作的key
            int key;
            // 70%概率访问热点数据，30%概率访问冷数据
            if (hotDataDist(gen)) {
                key = hotKeyDist(gen);  // 热点数据
            } else {
                key = HOT_KEYS + coldKeyDist(gen);  // 冷数据
            }

            // 30%概率进行写操作
            bool isPut = writeDist(gen);
            if (isPut) {
                // 执行put操作
                std::string value = "value" + std::to_string(key) + "_v" + std::to_string(op % 100);
                caches[cacheIdx]->put(key, value);
            } else {
                // 执行get操作并记录命中情况
                std::string result;
                getOperations[cacheIdx]++;
                if (caches[cacheIdx]->get(key, result)) {
                    hits[cacheIdx]++;
                }
            }
        }
    }

    // 打印测试结果
    printResults("热点数据访问测试", CAPACITY, getOperations, hits);
}

void testLoopPattern() {
    std::cout << "\n=== 测试场景2：循环扫描测试 ===" << std::endl;

    const int CAPACITY = 50;        // 缓存容量
    const int LOOP_SIZE = 100;      // 循环范围大小
    const int OPERATIONS = 200000;  // 总操作次数

    std::random_device rd;
    std::mt19937 gen(rd());

    // 写操作分布（20%概率写操作）
    std::bernoulli_distribution writeDist(0.2);
    // 随机跳跃分布（30%概率随机跳跃）
    std::bernoulli_distribution randomJumpDist(0.3);
    // 范围分布
    std::uniform_int_distribution<int> loopKeyDist(0, LOOP_SIZE - 1);
    std::uniform_int_distribution<int> outsideKeyDist(0, LOOP_SIZE - 1);

    LruCache<int, std::string> lru(CAPACITY);
    LfuAgingCache<int, std::string> lfu(CAPACITY);
    ArcCache<int, std::string> arc(CAPACITY);
    LruKCache<int, std::string> lruk(CAPACITY, LOOP_SIZE * 2, 2);
    LfuAgingCache<int, std::string> lfuAging(CAPACITY, 3000);

    std::array<Policy<int, std::string>*, 5> caches{&lru, &lfu, &arc, &lruk, &lfuAging};

    // 记录命中次数和总操作次数
    std::vector<int> hits(5, 0);
    std::vector<int> getOperations(5, 0);
    std::vector<std::string> names{"LRU", "LFU", "ARC", "LRU-k", "LFU-Aging"};

    // 为每种缓存算法运行相同的测试
    for (size_t cacheIdx = 0; cacheIdx < caches.size(); ++cacheIdx) {
        // 先预热一部分数据（只加载20%的数据）
        for (int key = 0; key < LOOP_SIZE / 5; ++key) {
            std::string value = "loop" + std::to_string(key);
            caches[cacheIdx]->put(key, value);
        }

        // 设置循环扫描的当前位置
        int currentPos = 0;

        // 交替进行读写操作，模拟真实场景
        for (int op = 0; op < OPERATIONS; ++op) {
            // 获取要操作的 key
            int key;

            // 按照不同模式选择键
            if (op % 100 < 60) {  // 60%顺序扫描
                key = currentPos;
                currentPos = (currentPos + 1) % LOOP_SIZE;
            } else if (randomJumpDist(gen)) {  // 30%随机跳跃
                key = loopKeyDist(gen);
            } else {  // 10%访问范围外数据
                key = LOOP_SIZE + outsideKeyDist(gen);
            }

            // 20%概率是写操作，80%概率是读操作
            bool isPut = writeDist(gen);
            if (isPut) {
                // 执行put操作，更新数据
                std::string value = "loop" + std::to_string(key) + "_v" + std::to_string(op % 100);
                caches[cacheIdx]->put(key, value);
            } else {
                // 执行get操作并记录命中情况
                std::string result;
                getOperations[cacheIdx]++;
                if (caches[cacheIdx]->get(key, result)) {
                    hits[cacheIdx]++;
                }
            }
        }
    }

    printResults("循环扫描测试", CAPACITY, getOperations, hits);
}

void testWorkloadShift() {
    std::cout << "\n=== 测试场景3：工作负载剧烈变化测试 ===" << std::endl;

    const int CAPACITY = 30;                  // 缓存容量
    const int OPERATIONS = 80000;             // 总操作次数
    const int PHASE_LENGTH = OPERATIONS / 5;  // 每个阶段的长度

    std::random_device rd;
    std::mt19937 gen(rd());

    // 读写操作分布（根据阶段动态设置）
    std::vector<std::bernoulli_distribution> writeDists = {
        std::bernoulli_distribution(0.15),  // 阶段1: 15%写入
        std::bernoulli_distribution(0.30),  // 阶段2: 30%写入
        std::bernoulli_distribution(0.10),  // 阶段3: 10%写入
        std::bernoulli_distribution(0.25),  // 阶段4: 25%写入
        std::bernoulli_distribution(0.20)   // 阶段5: 20%写入
    };

    // 范围分布
    std::uniform_int_distribution<int> hotKeyDist(0, 4);     // 热点：0-4
    std::uniform_int_distribution<int> midKeyDist(0, 44);    // 中等：5-49
    std::uniform_int_distribution<int> coldKeyDist(0, 349);  // 大范围：50-399

    LruCache<int, std::string> lru(CAPACITY);
    LfuAgingCache<int, std::string> lfu(CAPACITY);
    ArcCache<int, std::string> arc(CAPACITY);
    LruKCache<int, std::string> lruk(CAPACITY, 500, 2);
    LfuAgingCache<int, std::string> lfuAging(CAPACITY, 10000);

    std::array<Policy<int, std::string>*, 5> caches = {&lru, &lfu, &arc, &lruk, &lfuAging};

    // 记录命中次数和总操作次数
    std::vector<int> hits(5, 0);
    std::vector<int> getOperations(5, 0);
    std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};

    // 为每种缓存算法运行相同的测试
    for (size_t cacheIdx = 0; cacheIdx < caches.size(); ++cacheIdx) {
        // 先预热缓存，只插入少量初始数据
        for (int key = 0; key < 30; ++key) {
            std::string value = "init" + std::to_string(key);
            caches[cacheIdx]->put(key, value);
        }

        // 进行多阶段测试，每个阶段有不同的访问模式
        for (int op = 0; op < OPERATIONS; ++op) {
            // 确定当前阶段
            int phase = op / PHASE_LENGTH;

            // 根据不同阶段选择不同的访问模式生成key - 优化后的访问范围
            int key;
            if (op < PHASE_LENGTH) {  // 阶段1: 热点访问 - 热点数量5，使热点更集中
                key = hotKeyDist(gen);
            } else if (op < PHASE_LENGTH * 2) {  // 阶段2: 大范围随机 - 范围400，更适合30大小的缓存
                key = midKeyDist(gen);
            } else if (op < PHASE_LENGTH * 3) {  // 阶段3: 顺序扫描 - 保持100个键
                key = (op - PHASE_LENGTH * 2) % 100;
            } else if (op < PHASE_LENGTH * 4) {  // 阶段4: 局部性随机 - 优化局部性区域大小
                // 产生5个局部区域，每个区域大小为15个键，与缓存大小20接近但略小
                int locality = (op / 800) % 5;          // 调整为5个局部区域
                key = locality * 15 + hotKeyDist(gen);  // 每区域15个键
            } else {                                    // 阶段5: 混合访问 - 增加热点访问比例
                int r = gen() % 100;
                if (r < 40) {                     // 40%概率访问热点（从30%增加）
                    key = hotKeyDist(gen);        // 5个热点键
                } else if (r < 70) {              // 30%概率访问中等范围
                    key = 5 + midKeyDist(gen);    // 缩小中等范围为50个键
                } else {                          // 30%概率访问大范围（从40%减少）
                    key = 50 + coldKeyDist(gen);  // 大范围也相应缩小
                }
            }

            // 确定是读还是写操作
            bool isPut = writeDists[phase](gen);
            if (isPut) {
                // 执行写操作
                std::string value = "value" + std::to_string(key) + "_p" + std::to_string(phase);
                caches[cacheIdx]->put(key, value);
            } else {
                // 执行读操作并记录命中情况
                std::string result;
                getOperations[cacheIdx]++;
                if (caches[cacheIdx]->get(key, result)) {
                    hits[cacheIdx]++;
                }
            }
        }
    }

    printResults("工作负载剧烈变化测试", CAPACITY, getOperations, hits);
}

int main() {
    testHotDataAccess();
    testLoopPattern();
    testWorkloadShift();
}