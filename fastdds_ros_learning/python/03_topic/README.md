# 第 3 章 · 把 DDS Topic 包成 rclpy 风格的发布订阅（Python）

本目录与 `cpp/03_topic/` 逐行对应，差异说明见那边的 README，
这里只讲 Python 侧的文件和写法。

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
