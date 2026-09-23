# 第 1 章 · 裸 Fast DDS API（Python）

这一章**刻意不用任何自定义封装**，把 Fast DDS 实体的创建流程完整摊开。
本目录与 `cpp/01_basic_api/` 逐行对应，可以对照着读。

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
            ├── Publisher               发布侧的容器
            │     └── DataWriter        真正执行发送的实体
            │           write()
            │
            └── Subscriber              订阅侧的容器
                  └── DataReader        真正接收数据的实体
                        take_next_sample()
```

一句话记忆：**工厂造参与者，参与者造 Topic 和收发容器，容器造读写器。**

Python 里还有一个额外约束：**所有实体都要保持引用**（本章把它们绑在局部变量上）。
被垃圾回收的 SWIG 对象可能已经注册进 DDS，回收后会出现很难查的
「对象还在但收不到数据」。

### ② 两个进程怎么互相找到 —— 发现（Discovery）

DDS 是**去中心化**的：没有服务器帮你牵线，每个参与者在后台广播自己、
收听别人。整个过程不需要你写代码。

```
   进程 A（publisher.py）                   进程 B（subscriber_listener.py）
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
                ▼                                        ▼
     PublicationMatchedStatus                  SubscriptionMatchedStatus
        .current_count = 1                        .current_count = 1
                │                                        │
                │   ③ 匹配之后，数据才真正开始流动
                │ <═══════════ write() ═════════════════ │
```

**为什么必须等匹配？** reader 还没被发现就 `write()`，那份样本找不到收件人，
**直接被丢掉**，不报错也不重传。publisher.py 里的 `while True` 轮询
`current_count` 就是在等第 ② 步完成。

换个域号（`create_participant(1, ...)`）等于换个频道，
两个域的参与者**完全看不见**对方：

```
   域 0  ┌── A ──┐  ┌── B ──┐        域 1  ┌── C ──┐
         └───────┘  └───────┘              └───────┘
         A 和 B 能互相发现                    C 谁也看不见
         C 和 A、B 之间完全隔离
```

### ③ 数据从 write() 到 take_next_sample() 的完整路径

```
   publisher.py                       subscriber_listener.py
   ────────────                       ──────────────────────
   sample.index(3)
   writer.write(sample)
        │
        │  ① 序列化：由 HelloWorldPubSubType 完成
        ▼
   DataWriter 历史缓存   ② history().depth 决定存几条
        │
        │  ③ RTPS 协议分片、编号、必要时重传
        ▼
   ═══════════════════ 网络 / 共享内存 ═══════════════════
        │
        ▼
   DataReader 缓存       ④ on_data_available() 被回调
        │
        ▼
   reader.take_next_sample(sample, info)
        │
        │  ⑤ 反序列化回对象，并把这条从缓存里【删除】
        ▼
   sample.index() == 3
```

### ④ 两种取数方式：谁在哪个线程上干活

这是 listener 和轮询的**唯一本质区别**。

```
 ┌─ listener 方式（subscriber_listener.py）─────────────────────────────┐
 │                                                                       │
 │  Fast DDS 内部线程          on_data_available(reader)  ← 自动被调用   │
 │  ─────────────────  ────>   while reader.take_next_sample(...) == OK  │
 │                                   处理样本                            │
 │                                                                       │
 │  主线程                                                               │
 │  ────────────  while True: time.sleep(0.2)   ← 干等着，什么也不做    │
 │                                                                       │
 │  特点：回调是【并发】的，里面的共享数据要自己加锁                     │
 └───────────────────────────────────────────────────────────────────────┘

 ┌─ 轮询方式（subscriber_polling.py）───────────────────────────────────┐
 │                                                                       │
 │  Fast DDS 内部线程                                                    │
 │  ─────────────────   （只负责把数据放进 reader 缓存，不回调任何人）   │
 │                                                                       │
 │  主线程                                                               │
 │  ─────────────────   while True:                                      │
 │                          reader.wait_for_unread_message(1s)  ← 阻塞等 │
 │                          while reader.take_next_sample(...) == OK     │
 │                              处理样本                                 │
 │                                                                       │
 │  特点：控制流全在你手里，好读好调试；代价是循环和超时要自己管         │
 └───────────────────────────────────────────────────────────────────────┘
```

### ⑤ 一对多 / 多对多是怎么自然成立的

匹配是按 (Topic, 类型, QoS) 做的，**谁都可能匹配上谁**，不需要额外配置：

```
   一对多                                   多对多
   ──────                                   ──────
                 ┌── subscriber_listener                 ┌── 订阅端 1
   发布端 ───────┤                          发布端 1 ───┤
                 └── subscriber_polling                  │
                                                       ├── 订阅端 2
                                            发布端 2 ───┤
                                                       │
                                                       └── 订阅端 3
```

上面这些连线**全部由发现协议自动完成**。本项目验证过：一个发布端 +
两个订阅端同时跑，发布端会打印「已匹配 2 个订阅端」，两边各收到全部 10 条。

## 文件说明

| 文件 | 作用 |
|---|---|
| `BasicTypes.idl` | 数据类型定义，与 `cpp/01_basic_api/BasicTypes.idl` 内容完全相同。生成出 `BasicTypes.py` 模块 |
| `publisher.py` | **发布端**。按注释走一遍 `DomainParticipantFactory` → `create_participant` → `register_type` → `create_topic` → `create_publisher` → `create_datawriter` → `write` 的完整链路 |
| `subscriber_listener.py` | **订阅端（listener 回调）**。`create_datareader` 传 listener 对象，在 `on_data_available` 里取数据。回调跑在 DDS 内部线程上 |
| `subscriber_polling.py` | **订阅端（主线程轮询）**。`create_datareader` 传 `None`，主循环用 `wait_for_unread_message()` 阻塞等待。控制流全在 `main` 里 |
| `CMakeLists.txt` | 生成类型代码 + 用 SWIG 包成可 import 的 Python 模块 |

## 生成代码怎么被 import 进来

`BasicTypes.py` 是构建产物，不在源码目录里。每个脚本开头的三行就是在指路：

```python
_REPO_ROOT = Path(__file__).resolve().parents[2]     # python/<章节>/x.py → 仓库根
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "01_basic_api"))
import BasicTypes
```

**这不是共享封装**，只是「生成代码的存放位置」。所以：
必须先构建，再运行；而且每章只用自己那份生成代码（各章的模块名不同，不会串）。

另外注意区分两件事 —— 生成的文件名跟着 `.idl` 文件名走，
里面的类型名跟着 IDL 里的 `struct` 名走：
`BasicTypes.idl` → 模块 `BasicTypes.py`，但类型叫 `BasicTypes.HelloWorld`。

## 与 C++ 的三点用法差异

1. **每个实体由它的"父实体"创建**，而不是各自 `new`。
2. **QoS 不能直接写 `*_QOS_DEFAULT` 哨兵**，要先造一个空的 Qos 对象、
   用 `get_default_xxx_qos()` 填充它，再传给 `create_xxx()`。
3. 改 QoS 的顺序同样是「取默认值 → 改字段 → 用它创建」，
   因为 `get_default_xxx_qos()` 是整体赋值而不是合并字段。

## 两个容易踩的坑

**1. `wait_for_unread_message()` 返回 `bool` 不是 `ReturnCode_t`。**
它返回"有数据 `True` / 超时 `False`"。写成
`if reader.wait_for_unread_message(t) != fastdds.RETCODE_OK:` 的话，
因为 `RETCODE_OK` 是 0 而 `True` 是 1，判断恒成立 ——
结果是**每次有数据都跳过**，一条都收不到。这个 bug 不报错，只是静默地什么都不打印。

**2. listener 对象不能被垃圾回收。**
`create_datareader(topic, qos, listener)` 之后，reader 持有 listener 的引用，
并在自己的线程里调用它。如果这个 Python 对象被 GC 回收了，
回调会**静默失效** —— 不报错，只是再也收不到数据。
所以脚本里都把它绑在一个活到程序结束的变量上。

## 关于官方示例

eProsima 官方的 `HelloWorldExample.py` 里有两个常见错误，本项目修掉了，
并在代码注释里标了出来，可以作为反面教材对照：

- `on_data_available` 只取一条样本，没有 `while` 排空 —— 数据积压时处理不过来；
- 没有检查 `info.valid_data` —— 会把 dispose/unregister 这类生命周期事件
  当成正常消息打印出来。

## 构建与运行

```zsh
# 先保证 fastdds 绑定在 PYTHONPATH 上（见仓库根 README）
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"

cmake --build build --target BasicTypesWrapper
```

- 终端 A（先起）：`python3 python/01_basic_api/subscriber_listener.py`
- 终端 A（可选）：`python3 python/01_basic_api/subscriber_polling.py`
- 终端 B：`python3 python/01_basic_api/publisher.py`
