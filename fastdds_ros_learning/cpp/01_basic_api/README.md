# 第 1 章 · 裸 Fast DDS API

这一章**刻意不用任何自定义封装类**，把 Fast DDS 实体的创建流程完整摊开。
后面几章的封装类做的事，本质都是这里这套流程换了个写法。

## 图解

### ① 实体包含关系 —— DDS 的对象是一棵树

```
    DomainParticipantFactory            进程级单例，一切的入口
      │                                 get_instance()
      │
      └── DomainParticipant             参与者：本进程在某个 DDS 域里的身份
            │                           create_participant(域号, QoS)
            │
            ├── TypeSupport             类型：怎么把 HelloWorld 变成字节
            │     register_type()       ⚠ 必须在建 Topic 之前
            │
            ├── Topic                   话题：一个【名字】+ 一个【类型】
            │     create_topic()        ⚠ 名字和类型两端必须完全一致
            │
            ├── Publisher               发布侧的容器（不收发，只管本身的 QoS）
            │     │
            │     └── DataWriter        真正执行发送的实体
            │           write()
            │
            └── Subscriber              订阅侧的容器
                  │
                  └── DataReader        真正接收数据的实体
                        take_next_sample() / wait_for_unread_message()
```

一句话记忆：**工厂造参与者，参与者造 Topic 和收发容器，容器造读写器。**

清理时只需两步，中间那一层全由 `delete_contained_entities()` 连带回收：

```
    participant->delete_contained_entities();     ← 一次删掉 Topic / Publisher /
                                                    Subscriber / DataWriter / DataReader
    factory->delete_participant(participant);     ← 再删参与者本身
```

### ② 两个进程怎么互相找到 —— 发现（Discovery）

DDS 是**去中心化**的：没有任何"服务器"帮你牵线，每个参与者都在后台
广播自己的信息，并收听别人的。整个过程不需要你写代码。

```
   进程 A（basic_publisher）                进程 B（basic_subscriber_listener）
   ┌─ DomainParticipant  域 0 ─┐            ┌─ DomainParticipant  域 0 ─┐
   └────────────┬─────────────┘            └─────────────┬────────────┘
                │                                        │
                │   ① 后台发现协议自动交换信息（你没写这段代码）
                │ ─────────────────────────────────────> │
                │   "我有 HelloWorldTopic / HelloWorld 的 DataWriter"
                │ <───────────────────────────────────── │
                │   "我有 HelloWorldTopic / HelloWorld 的 DataReader"
                │                                        │
                │   ② 名字对得上吗？类型对得上吗？QoS 兼容吗？
                │      三个都满足 → 匹配成功
                │                                        │
                ▼                                        ▼
     PublicationMatchedStatus                  SubscriptionMatchedStatus
        .current_count = 1                        .current_count = 1
                │                                        │
                │   ③ 匹配之后，数据才真正开始流动
                │ <═══════════ write() ═════════════════ │
```

**为什么必须等匹配？** 如果 reader 还没被发现就 `write()`，那一份样本
找不到任何收件人，**直接被丢掉**，不报错也不重传（默认的 QoS 下）。
发布端里那个 `while (matched.current_count == 0)` 循环就是在等第 ② 步完成。

换个域号（`create_participant(1, ...)`）就等于换个频道，
两个域的参与者互相**完全看不见**。这是最省事的实验隔离手段：

```
   域 0  ┌── A ──┐  ┌── B ──┐        域 1  ┌── C ──┐
         └───────┘  └───────┘              └───────┘
         A 和 B 能互相发现                    C 谁也看不见
         C 和 A、B 之间完全隔离
```

### ③ 数据从 writer 走到 reader 的完整路径

`write()` 返回 `RETCODE_OK` 只代表**中间件收下了**，样本这时才刚进第一站。
后面每一段对应用户代码来说都是异步的：

```
   basic_publisher                    basic_subscriber_listener
   ───────────────                    ──────────────────────────
   sample.index(3)
   sample.message("...")
        │
        ▼
   writer->write(&sample)
        │
        │  ① 序列化：按 IDL 定义把结构体拍成字节流
        │     这一步由 fastddsgen 生成的 HelloWorldPubSubType 完成
        ▼
   ┌──────────────────┐
   │ DataWriter 的    │  ② 进本地历史缓存（history().depth 决定深度）
   │ 历史缓存          │     可靠传输时会一直留到对端确认
   └────────┬─────────┘
            │
            │  ③ RTPS 协议：分片、加序号、可能重传
            ▼
   ══════════════════════════════════════  网络 / 共享内存
            │
            ▼
   ┌──────────────────┐
   │ DataReader 的    │  ④ 进对端缓存，按 QoS 决定能存几条
   │ 缓存              │
   └────────┬─────────┘
            │
            │  ⑤ 通知：listener 被回调 / wait_for_unread_message 返回
            ▼
   reader->take_next_sample(&sample, &info)
        │
        │  ⑥ 反序列化回结构体，并把这条从缓存里【删除】
        ▼
   sample.index() == 3
```

第 ⑤ 步就是两种取数方式的分岔口 —— 见下一张图。

### ④ 两种取数方式：谁在哪个线程上干活

这是 listener 和轮询的**唯一本质区别**。

```
 ┌─ listener 方式（basic_subscriber_listener.cpp）───────────────────────┐
 │                                                                       │
 │  Fast DDS 内部线程          on_data_available(reader)  ← 自动被调用   │
 │  ─────────────────  ────>   while (take_next_sample(...) == OK)       │
 │                                   处理样本                            │
 │                                                                       │
 │  主线程                                                            │
 │  ────────────  while (g_running) sleep(200ms)   ← 干等着，什么也不做 │
 │                                                                       │
 │  特点：回调是【并发】的，里面的共享数据要自己加锁                     │
 │        不用管循环和超时，数据来了自动干活                             │
 └───────────────────────────────────────────────────────────────────────┘

 ┌─ 轮询方式（basic_subscriber_polling.cpp）─────────────────────────────┐
 │                                                                       │
 │  Fast DDS 内部线程                                                    │
 │  ─────────────────   （只负责把数据放进 reader 缓存，不回调任何人）   │
 │                                                                       │
 │  主线程                                                               │
 │  ─────────────────   while (g_running) {                              │
 │                          wait_for_unread_message(1s)  ← 阻塞等        │
 │                          while (take_next_sample(...) == OK)          │
 │                              处理样本                                 │
 │                      }                                                │
 │                                                                       │
 │  特点：控制流全在你手里，好读好调试                                   │
 │        代价是要自己管循环和超时                                       │
 └───────────────────────────────────────────────────────────────────────┘
```

**注意两边都必须用 `while` 排空**：通知只保证"此刻 reader 上有数据"，
不保证只有一条。一次只取一条的话，积压的数据要等下一次通知才处理得完，
高频率下发会越来越落后。

### ⑤ 一对多 / 多对多是怎么自然成立的

DDS 的发布订阅天然是**多对多**，不需要任何额外配置 ——
匹配是按 (Topic, 类型, QoS) 做的，谁都可能匹配上谁：

```
   一对多                                   多对多
   ──────                                   ──────
                 ┌── listener 订阅端                    ┌── 订阅端 1
   发布端 ───────┤                          发布端 1 ───┤
                 └── polling 订阅端                     │
                                                       ├── 订阅端 2
                                            发布端 2 ───┤
                                                       │
                                                       └── 订阅端 3
```

上面这些连线的建立**全部由发现协议自动完成**。本项目验证过：
一个发布端 + 两个订阅端同时跑，发布端 `matched.current_count` 会是 2，
两个订阅端各自收到全部 10 条。

## 文件说明

| 文件 | 作用 |
|---|---|
| `BasicTypes.idl` | 数据类型定义。一个 `struct HelloWorld { unsigned long index; string message; }`，`fastddsgen` 把它翻译成 `BasicTypes.hpp`（纯数据结构）和 `BasicTypesPubSubTypes.hpp`（DDS 类型适配类） |
| `basic_publisher.cpp` | **发布端**。注释按步骤标出了 `DomainParticipantFactory` → `create_participant` → `register_type` → `create_topic` → `create_publisher` → `create_datawriter` → `write` 的完整链路，以及三步清理 |
| `basic_subscriber_listener.cpp` | **订阅端（listener 回调）**。`create_datareader` 传 `this` 作监听器，在自己的 `on_data_available` 里取数据。回调跑在 DDS 内部线程上 |
| `basic_subscriber_polling.cpp` | **订阅端（主线程轮询）**。`create_datareader` 传 `nullptr`，主循环用 `wait_for_unread_message()` 阻塞等待。控制流全在 `main` 里，对初学者更直观 |
| `CMakeLists.txt` | 本章构建：生成类型代码 → 编译成 `basic_types` 库 → 编译三个可执行文件 |

两个订阅端是**对照实验**：数据路径完全一样，只有"谁在什么线程上取数据"不同。
建议同时开两个，用同一个发布端喂数据，观察输出。

## 学到的 API

| API | 说明 |
|---|---|
| `DomainParticipantFactory::get_instance()` | 进程级单例，一切的入口 |
| `create_participant(域号, QoS)` | 建参与者。域号相同的进程才能互相发现 |
| `TypeSupport::register_type()` | 注册类型。**必须先于 `create_topic`**，否则 `create_topic` 返回 `nullptr` |
| `create_topic(名字, 类型名, QoS)` | 把一个名字和一个类型绑在一起。两端名字必须完全一致 |
| `create_publisher()` / `create_subscriber()` | 发布/订阅的容器，与 `create_topic` 之间**没有**先后要求 |
| `create_datawriter()` / `create_datareader()` | 真正收发的实体。reader 的第三个参数决定用回调还是轮询 |
| `DataWriter::write()` | 发送。返回 `RETCODE_OK` 只表示中间件收下了，**不代表对端收到** |
| `DataReader::take_next_sample()` | 取一条并删除；`read_next_sample()` 取一条但保留 |
| `DataReader::wait_for_unread_message()` | 阻塞等待。**返回 `bool` 而不是 `ReturnCode_t`** |
| `delete_contained_entities()` + `delete_participant()` | 清理。前者会连带删掉下面所有实体 |

## 两个容易踩的坑

**1. QoS 的顺序** —— 想改 QoS 必须是「取默认值 → 改字段 → 用它创建」：

```cpp
DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
publisher->get_default_datawriter_qos(qos);   // ① 取
qos.history().depth = 10;                     // ② 改
writer = publisher->create_datawriter(topic, qos);   // ③ 建
```

`get_default_*_qos()` 是**整体赋值**而不是合并字段。把 ② 写在 ① 前面，
你设的值会被随后的赋值覆盖掉。官方 `hello_world` 示例这里就写反了。

**2. `wait_for_unread_message()` 的返回类型** —— 它是 `bool`（有数据 `true` / 超时 `false`），
不是 `ReturnCode_t`。写成 `if (RETCODE_OK != reader->wait_for_unread_message(t))` 的话，
因为 `RETCODE_OK` 是 0 而 `true` 是 1，判断恒为真 —— 结果是**每次有数据都跳过**，
一条都收不到。这个 bug 不会报错，只会静默地什么都不打印。

## 构建与运行

```zsh
cmake --build build --target basic_publisher basic_subscriber_listener basic_subscriber_polling
```

先起订阅端，再起发布端：

- 终端 A：`./build/cpp/01_basic_api/basic_subscriber_listener`
- 终端 A（可选）：`./build/cpp/01_basic_api/basic_subscriber_polling`
- 终端 B：`./build/cpp/01_basic_api/basic_publisher`

发布端发 10 条、间隔 1 秒，发完自己退出；订阅端按 Ctrl-C 退出。
Python 版见 `python/01_basic_api/`。
