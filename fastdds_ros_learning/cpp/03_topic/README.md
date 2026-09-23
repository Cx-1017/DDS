# 第 3 章 · 把 DDS Topic 包成 rclcpp 风格的发布订阅

第 2 章讲的是"用 DDS 拼出一个 ROS 概念"，这一章反过来：
DDS 的发布订阅本来就和 ROS 的话题是一回事，所以重点在于**把 API 形状做成 ROS 的样子**，
并说清楚两者在语义上的差异。

## 文件说明

| 文件 | 作用 |
|---|---|
| `TopicTypes.idl` | 一个 `struct Chatter { unsigned long index; string text; }`，对应 ROS 2 的一个 `.msg` |
| `node.hpp` | 三个 ROS 风格的类：<br>`Node` —— 持有一个 `DomainParticipant`，提供 `create_publisher()` / `create_subscription()` / `spin()`<br>`Publisher` —— 包 `DataWriter`，对外只有 `publish()`<br>`Subscription` —— 包 `DataReader`，同时继承 `DataReaderListener`，在 `on_data_available()` 里把样本交给用户回调 |
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
