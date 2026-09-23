# 第 3 章 · 把 DDS Topic 包成 rclcpp 风格的发布订阅

第 2 章讲的是"用 DDS 拼出一个 ROS 概念"，这一章反过来：
DDS 的发布订阅本来就和 ROS 的话题是一回事，所以重点在于**把 API 形状做成 ROS 的样子**，
并说清楚两者在语义上的差异。

## 图解

### ① 这一章其实没造新东西，只是换了层皮

和第 2、4、5 章不同，话题功能在 DDS 里是**原生的**。所以这里的映射是
**一对一的**，中间没有"用两条 Topic 拼出第三条路"这种协议设计：

```
   rclcpp / rclpy 写法                          对应的 DDS 实体
   ─────────────────────                        ──────────────────────

   std::make_shared<rclcpp::Node>("talker")  →  DomainParticipant
                                                  （域 0）

   create_publisher<Chatter>(topic, 10)      →  register_type + create_topic
                                                 + create_publisher
                                                 + create_datawriter
                                                 （4 步！这就是为什么需要封装）

   create_subscription<Chatter>(topic, 10,cb)→  register_type + create_topic
                                                 + create_subscriber
                                                 + create_datareader

   pub->publish(msg)                         →  DataWriter::write()
   rclcpp::spin(node)                        →  （没有对应物，见下一张图）

   ↑ ROS 一行，DDS 四步。封装的意义就是把                  ↑
     这四步收进一个构造函数里                   这个「没有对应物」是关键差异
```

### ② 什么条件下两个端点才会匹配

这是话题机制最核心的规则。**三个条件全部满足**才会建立连接：

```
                   发布端（talker）                    订阅端（listener）
                   ────────────────                    ──────────────────

   ① Topic 名字       "/demo/chatter"        ==?        "/demo/chatter"        ✓
   ② 类型名           "Chatter"              ==?        "Chatter"              ✓
   ③ QoS 兼容         RELIABLE              兼容?       RELIABLE               ✓
                     history.depth = 10                history.depth = 10
                            │                                  │
                            └──────────────┬───────────────────┘
                                           ▼
                                    匹配成功，建立连接
                                    writer 的 PublicationMatchedStatus
                                         .current_count = 1

   ─────────────────────── 任意一条不满足 ───────────────────────

   ① 名字不同             "/demo/chatter"      ≠         "/demo/other"          ✗
      → 静默不匹配。不报错、不警告，两边各自跑到超时 —— 这是最常见的坑

   ② 类型名不同           "Chatter"            ≠         "ChatterV2"            ✗
      → 同样静默不匹配

   ③ QoS 不兼容           BEST_EFFORT         与?        RELIABLE              ✗
      → 同样不匹配（可靠性是"不低于"关系，RELIABLE 的 reader
         不接受 BEST_EFFORT 的 writer）
```

**为什么强调"静默"**：DDS 是去中心化的，没有中心节点能告诉你
"对面在，但你俩条件对不上"。两边看到的都只是 `current_count == 0`。
排查匹配问题要看 `fastdds discovery -i 0 -l` 的输出，而不是等报错。

### ③ 线程模型：这是和 ROS 最本质的差异

同样叫 `spin()`，两边的含义**完全不同**：

```
 ┌─ ROS 2（rclcpp / rclpy）──────────────────────────────────────────────┐
 │                                                                       │
 │   中间件线程      收到数据 → 放进【回调队列】→ 通知 executor            │
 │       │                                                               │
 │       │                            （这里就结束了，回调还没跑）          │
 │       ▼                                                               │
 │   主线程          rclcpp::spin(node)                                  │
 │                   └─ 从回调队列里取任务 → 执行你的 callback            │
 │                                                                       │
 │   ★ callback 是被 spin() 驱动的。不 spin，回调永远不跑。               │
 └───────────────────────────────────────────────────────────────────────┘

 ┌─ 本章（Fast DDS）─────────────────────────────────────────────────────┐
 │                                                                       │
 │   中间件线程      收到数据 → 直接调用 on_data_available()              │
 │                   └─ 在里面执行你的 callback                          │
 │                                                                       │
 │   ★ callback 跑在【中间件线程】上，和主线程无关。                      │
 │                                                                       │
 │   主线程          node.spin()                                         │
 │                   └─ while (running_) sleep(100ms)                    │
 │                       ↑ 纯粹是把主线程卡住，防止 main 返回、节点析构     │
 │                                                                       │
 │   ★ 不调 spin()，回调照样跑。spin() 在这边只是个"别退出"的等待。        │
 └───────────────────────────────────────────────────────────────────────┘
```

由此推出两条实践约束：
1. **回调里不能长时间阻塞** —— 会拖住中间件的收发线程；
2. **回调里的共享数据要自己加锁** —— 它是并发调用的，ROS 那边
   由 executor 串行化，这里没有。

### ④ 一对多 / 多对多

和第 1 章一样，匹配是按 (Topic, 类型, QoS) 做的，**谁都可能匹配上谁**：

```
   一个 talker，两个 listener                  两个 talker，一个 listener
   ─────────────────────────                  ──────────────────────────
                    ┌── listener 1                              ┌── talker 1
   talker ──────────┤                           listener ──────┤
                    └── listener 2                              └── talker 2

   两个 listener 都收到全部样本                 listener 收到两条流的样本，
   writer 的 current_count == 2                按 index 交错

   ↑ 这些连线全部由发现协议自动建立，零配置
```

### ⑤ Node 类在 DDS 里持有什么

```
   Node("talker")
     │
     ├── DomainParticipant（域 0）    ← 构造函数里建，析构函数里删
     │     └── register_type(ChatterPubSubType)  ← 只注册一次，
     │                                             重复注册同名类型会返回错误
     │
     ├── create_publisher("/demo/chatter", depth=10)
     │     ├── create_topic(名字, 类型名)          ← 复用上面注册的类型
     │     ├── create_publisher()                  ← 懒创建，整个节点共用一个
     │     └── create_datawriter(topic, qos)
     │           └── 包成 std::shared_ptr<Publisher> 返回
     │
     ├── create_subscription("/demo/chatter", cb, depth=10)
     │     ├── create_topic(名字, 类型名)
     │     ├── create_subscriber()                 ← 懒创建，共用一个
     │     └── create_datareader(topic, qos, sub.get())
     │           └── 把 Subscription 自己当 listener 传进去
     │
     └── 两个 vector 持有所有创建过的端点
           └─ ⚠ Subscription 尤其重要：DataReader 只持有它的裸指针，
              提前析构会让中间件线程回调到已释放的内存
```

## 文件说明

| 文件 | 作用 |
|---|---|
| `TopicTypes.idl` | 一个 `struct Chatter { unsigned long index; string text; }`，对应 ROS 2 的一个 `.msg` |
| `node.hpp` | 三个 ROS 风格的类：`Node`（持有一个 `DomainParticipant`，提供 `create_publisher()` / `create_subscription()` / `spin()`）；`Publisher`（包 `DataWriter`，对外只有 `publish()`）；`Subscription`（包 `DataReader`，同时继承 `DataReaderListener`，在 `on_data_available()` 里把样本交给用户回调） |
| `talker.cpp` | 发布端入口（对应 ROS 2 教程的 talker） |
| `listener.cpp` | 订阅端入口（对应 ROS 2 教程的 listener） |
| `CMakeLists.txt` | 本章构建 |

## 与 rclcpp 的对应关系

| rclcpp | 本章 |
|---|---|
| `std::make_shared<rclcpp::Node>("talker")` | `Node node("talker")` |
| `node->create_publisher<Chatter>(topic, 10)` | `node.create_publisher(topic, 10)` |
| `node->create_subscription<Chatter>(topic, 10, cb)` | `node.create_subscription(topic, cb, 10)` |
| `pub->publish(msg)` | `pub->publish(msg)` |
| `rclcpp::spin(node)` | `node.spin()` —— **含义不同，见下** |

## 三个必须知道的差异

**1. `spin()` 的含义完全不同。**
rclcpp 的 `spin()` 是主线程去跑回调队列，回调确实是**被 spin 驱动**的。
DDS 的回调跑在中间件自己的线程上，这里的 `spin()` 只是把主线程阻塞住，
防止 `main` 返回、节点被析构。**就算不调 `spin()`，回调照样会被触发。**

**2. 回调运行在 DDS 线程上。** 所以：
- 不要在回调里做长时间阻塞的操作，会拖住中间件的收发线程；
- 回调里碰到的共享数据要自己加锁（本章只是打印，不用加）。

**3. 名字不做任何加工。** ROS 2 有 namespace、remapping、命名规则那一套；
这里的 Topic 名就是 DDS 上真正的名字。

## 一个 C++ 特有的坑

`lesson03::Publisher`（本章的封装类）和 `eprosima::fastdds::dds::Publisher`
（DDS 的"发布侧容器"实体）**重名**。因为文件里有 `using namespace eprosima::fastdds::dds;`，
在 `lesson03` 内部裸写 `Publisher` 会解析到封装类上。
所以 `Node` 里提到 DDS 那个时写了全限定名，代码里也有注释标出这一点。

## 关于 QoS

`create_publisher(topic, history_depth)` 的第二个参数对应 ROS 2 的 qos 深度。
内部实现的顺序是「取默认 QoS → 改 `history().depth` → 创建」——
**顺序不能反**，`get_default_datawriter_qos()` 是整体赋值而不是合并字段。

## 构建与运行

```zsh
cmake --build build --target topic_talker topic_listener
```

- 终端 A（先起）：`./build/cpp/03_topic/topic_listener`
- 终端 B：`./build/cpp/03_topic/topic_talker`

预期：listener 的回调被调用 10 次，每次打印一条 `index` 和 `text`。
可以通过 `fastdds discovery -i 0 -l` 观察发现过程。

Python 版见 `python/03_topic/`。
