# 第 1 章 · 裸 Fast DDS API

这一章**刻意不用任何自定义封装类**，把 Fast DDS 实体的创建流程完整摊开。
后面几章的封装类做的事，本质都是这里这套流程换了个写法。

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
