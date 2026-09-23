# 第 3 章 · 把 DDS Topic 包成 rclpy 风格的发布订阅（Python）

本目录与 `cpp/03_topic/` 逐行对应，差异说明见那边的 README，
这里只讲 Python 侧的文件和写法。

## 图解

### ① 这一章其实没造新东西，只是换了层皮

话题功能在 DDS 里是**原生的**，所以映射是**一对一的**，
不像第 2/4/5 章那样要"用几条 Topic 拼出一个协议"：

```
   rclpy 写法                                    对应的 DDS 实体
   ──────────                                    ──────────────────────

   rclpy.create_node("talker")                →  DomainParticipant（域 0）

   node.create_publisher(Chatter, topic, 10)  →  register_type
                                                 + create_topic
                                                 + create_publisher
                                                 + create_datawriter

   node.create_subscription(Chatter,topic,cb,10) → register_type
                                                 + create_topic
                                                 + create_subscriber
                                                 + create_datareader

   pub.publish(msg)                           →  DataWriter.write()
   rclpy.spin(node)                           →  （没有对应物，见下一张图）

   ↑ Python 一行，DDS 四步。封装的意义               ↑
     就是把那四步收进一个构造函数里            这个「没有对应物」是关键差异
```

### ② 什么条件下两个端点才会匹配

**三个条件全部满足**才会建立连接：

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

   ─────────────────────── 任意一条不满足 ───────────────────────

   ① 名字不同             "/demo/chatter"      ≠         "/demo/other"          ✗
      → 静默不匹配。不报错、不警告，两边各自跑到超时 —— 这是最常见的坑

   ② 类型名不同           "Chatter"            ≠         "ChatterV2"            ✗
   ③ QoS 不兼容           BEST_EFFORT         与?        RELIABLE              ✗
```

**为什么强调"静默"**：DDS 是去中心化的，没有中心节点能告诉你
"对面在，但你俩条件对不上"。两边看到的都只是 `current_count == 0`。
排查匹配问题要看 `fastdds discovery -i 0 -l` 的输出，而不是等报错。

### ③ 线程模型：这是和 ROS 最本质的差异

同样叫 `spin()`，两边的含义**完全不同**：

```
 ┌─ ROS 2（rclpy）───────────────────────────────────────────────────────┐
 │                                                                       │
 │   中间件线程      收到数据 → 放进【回调队列】→ 通知 executor            │
 │       │                            （这里就结束了，回调还没跑）          │
 │       ▼                                                               │
 │   主线程          rclpy.spin(node)                                    │
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
 │                   └─ while self._running: time.sleep(0.1)             │
 │                       ↑ 纯粹是把主线程卡住，防止程序退出、节点析构       │
 │                                                                       │
 │   ★ 不调 spin()，回调照样跑。spin() 在这边只是个"别退出"的等待。        │
 └───────────────────────────────────────────────────────────────────────┘
```

由此推出两条实践约束：
1. **回调里不能长时间阻塞** —— 会拖住中间件的收发线程；
2. **回调里的共享数据要自己加锁** —— 它是并发调用的，ROS 那边
   由 executor 串行化，这里没有。

### ④ Python 特有的一个坑：每个样本都要新建对象

`on_data_available` 里是**每轮新建**一个 `Chatter()` 再取数，
而不是像第 1 章那样复用一个对象：

```
   复用同一个对象（第 1 章的写法）             每轮新建（本章的写法）
   ─────────────────────────────             ──────────────────────
   sample = Chatter()                        while True:
   while take_next_sample(sample, info):         sample = Chatter()
       callback(sample)  ← 传引用                 if take_next_sample(...) != OK: break
             │                                    callback(sample)  ← 传引用
             ▼                                          │
   回调如果存了引用，                          回调存了引用也没问题，
   循环几轮后所有引用                           每个引用指向各自的对象
   都指向【最后一条】数据 ⚠

   ↑ 第 1 章在循环里直接 print，不复用没问题；
     本章要把样本交给用户回调，回调可能把引用存起来，所以必须新建。
```

### ⑤ Node 类在 DDS 里持有什么

```
   Node("talker")
     │
     ├── DomainParticipant（域 0）    ← 构造函数里建，destroy() 里删
     │     └── register_type(ChatterPubSubType)  ← 只注册一次
     │
     ├── create_publisher("/demo/chatter", history_depth=10)
     │     ├── create_topic(名字, 类型名)
     │     ├── create_publisher()          ← 懒创建，整个节点共用一个
     │     └── create_datawriter(topic, writer_qos)
     │
     ├── create_subscription("/demo/chatter", callback, history_depth=10)
     │     ├── create_topic(名字, 类型名)
     │     ├── create_subscriber()         ← 懒创建，共用一个
     │     └── create_datareader(topic, reader_qos, sub)   ← sub 自己当 listener
     │
     └── 两个 list 持有所有创建过的端点（self._publishers / self._subscriptions）
           └─ ⚠ Subscription 尤其重要：DataReader 持有它的引用，
              被垃圾回收后回调会【静默失效】—— 不报错，只是收不到数据
```

## 文件说明

| 文件 | 作用 |
|---|---|
| `TopicTypes.idl` | 一个 `struct Chatter { unsigned long index; string text; }`，对应 ROS 2 的一个 `.msg` |
| `node.py` | 三个 ROS 风格的类：<br>`Node` —— 持有一个 `DomainParticipant`，提供 `create_publisher()` / `create_subscription()` / `spin()`<br>`Publisher` —— 包 `DataWriter`，对外只有 `publish()`<br>`Subscription` —— 包 `DataReader`，同时继承 `DataReaderListener`，在 `on_data_available()` 里把样本交给用户回调 |
| `talker.py` | 发布端入口（对应 ROS 2 教程的 talker） |
| `listener.py` | 订阅端入口（对应 ROS 2 教程的 listener） |
| `CMakeLists.txt` | 生成 `TopicTypes.py` + SWIG 模块 |

## 与 rclpy 的对应关系

| rclpy | 本章 |
|---|---|
| `rclpy.create_node("talker")` | `Node("talker")` |
| `node.create_publisher(Chatter, topic, 10)` | `node.create_publisher(topic, 10)` |
| `node.create_subscription(Chatter, topic, cb, 10)` | `node.create_subscription(topic, cb, 10)` |
| `pub.publish(msg)` | `pub.publish(msg)` |
| `rclpy.spin(node)` | `node.spin()` —— **含义不同，见下** |

## 三个必须知道的差异

**1. `spin()` 的含义完全不同。**
rclpy 的 `spin()` 是主线程去跑回调队列，回调确实是**被 spin 驱动**的。
DDS 的回调跑在中间件自己的线程上，这里的 `spin()` 只是把主线程阻塞住，
防止程序退出、节点被析构。**就算不调 `spin()`，回调照样会被触发。**

**2. 回调运行在 DDS 线程上。** 所以不要在回调里做长时间阻塞的操作（会拖住
中间件的收发线程），里面碰到的共享数据要自己加锁。

**3. 名字不做任何加工。** ROS 2 有 namespace、remapping、命名规则那一套；
这里的 Topic 名就是 DDS 上真正的名字。

## Python 侧的两个实现细节

**每轮循环新建样本对象。** `Subscription.on_data_available` 里每次都
`TopicTypes.Chatter()` 造一个新对象再取数，而不是复用同一个（第 1 章是复用的）。
区别在于这里要把样本交给**用户回调**，而回调可能把引用存起来 ——
复用的话那些引用最终都会指向同一条数据。

**节点持有所有创建过的端点。** `Node` 把 `Publisher` 和 `Subscription`
都存在列表里保活。尤其是 `Subscription` —— 它作为 listener 被 DataReader
持有引用，提前被垃圾回收会导致 DDS 线程回调到已释放的对象。

## 构建与运行

```zsh
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"
cmake --build build --target TopicTypesWrapper
```

- 终端 A（先起）：`python3 python/03_topic/listener.py`
- 终端 B：`python3 python/03_topic/talker.py`

预期：listener 的回调被调用 10 次，每次打印一条 `index` 和 `text`。
