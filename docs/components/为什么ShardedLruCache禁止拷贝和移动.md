这段文件内容是一份关于 C++ 语义问题的详细技术笔记，讨论的是 **“lambda 捕获 `this` + 隐式移动构造函数”** 这个陷阱。下面我按原文的逻辑，逐层解释它在说什么。

---

## 一、问题的本质

一个对象（`ShardedLruCache`）内部持有若干**堆上的分片**（`LruCache`），每个分片里存着一个回调（`std::function`），这个回调是一个 **lambda**，它捕获了 `this`——也就是**外层容器对象的地址**。

关键点在于：这是一个**“子指向父”的反向引用**。

```
用户回调 (std::function)
   ▲
   │ 存于
[ShardedLruCache @ 0x1000] 的 Policy::leaveCallback_   ← A
   ▲
   │ 被捕获为裸 this 指针
   │
分片里的 lambda（每个分片一个）
   ▲ 存于
[LruCache @ 0x5000] 的 leaveCallback_   ← B（在堆上，永不移动）
```

也就是说，分片里保存的回调，指向的是**外层容器的地址**。

---

## 二、发生 move 时到底发生了什么

```cpp
ShardedLruCache b = std::move(a);  // a 在 0x1000，b 在 0x2000
```

编译器隐式生成的移动构造函数会**逐成员**搬移：

1. **基类 `Policy`**：因为 `Policy` 有用户声明的虚析构函数，它没有移动构造函数，于是基类走**拷贝构造**——`b` 里的回调是 `a` 里回调的一份副本。
2. **`vector<unique_ptr<LruCache>>`**：vector 的 move 只是把 `unique_ptr` 里的**裸指针值**（0x5000、0x6000）复制到 `b` 的 vector，然后把 `a` 的 vector 清空。**堆上的 `LruCache` 对象原地不动，一个字节都没变**——它们内部 lambda 捕获的 `this` 仍然等于 0x1000。
3. `hashFunc_` 等正常移动。

搬完之后的状态：

- `b`（0x2000）：拥有分片 + 一份用户回调
- 分片（0x5000/0x6000）：lambda 里记住的却还是 **0x1000（a 的地址）**
- `a`（0x1000）：被搬空的壳，基类里还留着一份回调副本

**核心矛盾**：对象搬家了，但分片里 lambda 记住的旧地址没搬——引用的方向错了。

---

## 三、两种失败模式

### 失败模式 1：静默丢事件 / 回调错乱

如果 `a` 还活着，淘汰事件会走：

```
分片淘汰 → 分片 lambda → a->notifyLeave(...) → 触发 a 里的回调副本
```

看起来一切正常！这才是最阴险的地方——它**能通过随手测试**。直到你：

```cpp
b.setLeaveCallback(新回调);  // 改的是 b 的基类（0x2000）
```

分片永远路由不到那里，**新回调永不生效，旧回调一直触发**。如果旧回调持有已失效的上下文，就是逻辑炸弹。

### 失败模式 2：悬垂 `this` → UB

`a` 析构后（函数返回、`vector` 扩容、`optional` 重装等），0x1000 的内存被回收：

```
b.put(...) → 淘汰 → lambda 经悬垂 this 读已释放内存里的 std::function → UB
```

可能是崩溃，也可能是静默的内存破坏。

**最阴的场景是 vector 扩容**：

```cpp
std::vector<ShardedLruCache<int,int>> v;
v.emplace_back(4, 2);
v.emplace_back(8, 4);  // 扩容 → move 第一个元素 → 其分片捕获的 this 悬垂
```

分片本身是 `unique_ptr`，扩容 move 也只搬指针，分片稳定——于是系统看起来对扩容“免疫”，**唯一断掉的就是那条反向引用**。

---

## 四、为什么编译器救不了你

- 隐式 move 是**逐成员**的：vector 移动 ✓、hashFunc 移动 ✓——每个成员单独看都“正确”。编译器无法知道“分片深处的 lambda 里捕获了本对象的地址”。
- lambda 捕获的 `this` 是闭包创建那一刻的指针值，存在 `std::function` 里，之后**不可枚举、不可重绑定**。move 之后没有任何机制能“进到”每个分片把 0x1000 改成 0x2000。
- 这类问题统称 **“回调捕获 this”**，同族问题还有把 `this` 存进别的对象/观察者列表而不随 move 修正。

---

## 五、为什么 `LruKCache` 天然免疫（对照）

`LruKCache` 把子缓存作为**直接成员**（`mainCache_`、`historyList_`），而 `LruCache` 含 `std::mutex`，**不可拷贝也不可移动**。于是 `LruKCache` 的隐式 move 自动被定义为 deleted——**白捡了安全性**。

而 `ShardedLruCache` 用了 `unique_ptr` 间接层：`vector<unique_ptr<LruCache>>` 是可移动的（即使 `LruCache` 本身不可移动），于是容器可移动、被指对象不可移动，**缝隙就此打开**。

这是整段内容里最有价值的洞察之一。

---

## 六、`=delete` 四件套如何从根上解决

```cpp
ShardedLruCache(const ShardedLruCache&) = delete;
ShardedLruCache& operator=(const ShardedLruCache&) = delete;
ShardedLruCache(ShardedLruCache&&) = delete;
ShardedLruCache& operator=(ShardedLruCache&&) = delete;
```

本质是把 **“对象地址终身不变”从约定升级为编译期不变量**。任何试图让对象搬家的代码直接编译失败，错误指向精确调用点，**零运行时开销**。

---

## 七、真正需要移动时的两个正经方案

1. **手写 move 构造/赋值**：搬完 vector 后，对每个分片重新 `setLeaveCallback`（新 lambda 捕获新 `this`）。可行，但要写对两处（ctor + assign），且搬空的源对象要保持可析构安全。
2. **间接层**：分片回调不捕 `this`，而捕获 `shared_ptr<SharedState>`，state 里放回调；对象随便搬，`shared_ptr` 保证存活。代价：一次堆分配 + 一层间接。

---

## 一句话总结

> 问题的根源不是 move 本身，而是**一条“子指父”的反向裸指针引用**：分片在堆上原地不动，外层容器却搬了家，分片里 lambda 捕获的旧 `this` 就成了错地址——轻则回调错乱、静默丢事件，重则悬垂 `this`、use-after-free。`=delete` 四件套把“对象地址终身不变”焊死为编译期不变量，是这类长生命周期服务对象的标准解法。