# 第 4 章 · 用 DDS 实现 ROS 2 风格的参数系统

## 参数在 DDS 上是什么

参数系统 = **服务的骨架 + 一条单向广播通道**。

```
客户端 --ParamRequest-->  /demo/parameters/request  --> 服务端
客户端 <--ParamResponse-- /demo/parameters/response <-- 服务端
所有人 <--ParamEvent----- /demo/parameter_events    <-- 服务端
```

前两条和第 2 章的服务完全同构（声明/读取/修改就是三种请求）。
第三条是关键区别：`parameter_events` **不是谁请求的响应，而是"参数变了"的广播**。
任何订阅了它的进程都能感知到参数变化，不需要它请求过什么 ——
这正是 DDS 发布订阅"一对多"特性的用武之地。

## 文件说明

| 文件 | 作用 |
|---|---|
| `ParameterTypes.idl` | 一个 `enum ParamOp` + 三个 struct：`ParamRequest` / `ParamResponse` / `ParamEvent`。注释里说明了 IDL enum 在 C++ 和 Python 两端**生成的写法不一样** |
| `parameter.hpp` | 三个类：`Endpoint<>`（最小端点，全章唯一直接碰 DDS 的地方）、`ParameterServer`（含参数表和三条通道）、`ParameterClient`（`declare_parameter` / `get_parameter` / `set_parameter`） |
| `server.cpp` | 参数服务端入口 |
| `client.cpp` | 参数客户端入口。演示完整生命周期：声明 → 读 → 改 → 再读，外加一次"读不存在的参数" |
| `monitor.cpp` | **只订阅 `parameter_events` 的监听程序**。单独开一个终端跑它，再跑 client 改参数，就能看到广播通知 |
| `CMakeLists.txt` | 本章构建 |

## 与 rclcpp 的对应关系

| rclcpp | 本章 |
|---|---|
| `node->declare_parameter("speed", 1)` | `client.declare_parameter("speed", 1, out)` |
| `node->get_parameter("speed")` | `client.get_parameter("speed", out)` |
| `node->set_parameter({"speed", 5})` | `client.set_parameter("speed", 5)` |
| `node->add_on_set_parameters_callback(cb)` | 订阅 `/demo/parameter_events`（见 `monitor.cpp`） |

## 关键点

- **为什么用 `enum ParamOp` 而不是 `0/1/2`**：IDL 的 enum 生成成两端的具名常量，
  不用靠注释记哪个数字代表什么。但两端的写法不同：
  - C++：`enum class` 有作用域 → `ParamOp::PARAM_GET`
  - Python：SWIG 把枚举类型名当前缀 → `ParameterTypes.ParamOp_PARAM_GET`

  裸写 `PARAM_GET` 在 C++ 里编译不过。这是跨语言调试时最容易卡住的点之一。

- **失败要能被调用方看见**。读一个没声明过的参数时，服务端如实返回
  `success = false`，而不是悄悄给个 0 —— 后者会让调用方以为参数存在，
  把 bug 藏到很远的地方。

- **参数表就是应用层的一个 `std::map`**。DDS 只负责传输，
  存储和语义全在应用层。

## 构建与运行

```zsh
cmake --build build --target param_server param_client param_monitor
```

- 终端 A（先起）：`./build/cpp/04_parameter/param_server`
- 终端 B：`./build/cpp/04_parameter/param_client`
- 终端 C（可选）：`./build/cpp/04_parameter/param_monitor`

预期：客户端打印 declare/get/set/get 四个值（set 之后 get 应该拿到 5），
并有一次"读不存在的参数"失败；终端 C 在参数被修改时打印一条 `[事件]`。

> 注意：连续运行两次 client 时，第二次的 `declare` 会保留第一次设的 5
> （`declare` 用的是 setdefault 语义），所以打印出来不是 1。

Python 版见 `python/04_parameter/`。
