# 第 4 章 · 用 DDS 实现 ROS 2 风格的参数系统（Python）

本目录与 `cpp/04_parameter/` 逐行对应，协议说明见那边的 README。

## 参数在 DDS 上是什么

```
客户端 --ParamRequest-->  /demo/parameters/request  --> 服务端
客户端 <--ParamResponse-- /demo/parameters/response <-- 服务端
所有人 <--ParamEvent----- /demo/parameter_events    <-- 服务端
```

前两条和第 2 章的服务同构。第三条是**广播**：不是谁请求的响应，
而是"参数变了"的通知，任何订阅它的进程都能收到。

## 文件说明

| 文件 | 作用 |
|---|---|
| `ParameterTypes.idl` | `enum ParamOp` + 三个 struct。注释里说明了 IDL enum 在两端**生成的写法不一样** |
| `parameter.py` | 三个类：`Endpoint`（最小端点）、`ParameterServer`（含参数表和三条通道）、`ParameterClient`（`declare_parameter` / `get_parameter` / `set_parameter`） |
| `server.py` | 参数服务端入口 |
| `client.py` | 参数客户端入口。演示完整生命周期：声明 → 读 → 改 → 再读，外加一次"读不存在的参数" |
| `monitor.py` | **只订阅 `parameter_events` 的监听程序**。单独开一个终端跑它，再跑 client 改参数，就能看到广播通知 |
| `CMakeLists.txt` | 生成 `ParameterTypes.py` + SWIG 模块 |

## 与 rclpy 的对应关系

| rclpy | 本章 |
|---|---|
| `node.declare_parameter("speed", 1)` | `client.declare_parameter("speed", 1)` → `(success, value)` |
| `node.get_parameter("speed").value` | `client.get_parameter("speed")` → `(success, value)` |
| `node.set_parameters([Parameter("speed", 5)])` | `client.set_parameter("speed", 5)` |
| `node.add_on_set_parameters_callback(cb)` | 订阅 `/demo/parameter_events`（见 `monitor.py`） |

Python 这边客户端方法返回 `(success, value)` 元组，而不是像 C++ 那样传引用出参 ——
这是 Python 里更自然的写法，也顺便让"失败"必须被调用方看到。

## 关键点

- **enum 在 Python 里的写法**：IDL 的 `enum ParamOp` 被 SWIG 展开成平铺的模块级常量，
  名字带上了枚举类型名前缀：`ParameterTypes.ParamOp_PARAM_GET`
  （C++ 那边是 `ParamOp::PARAM_GET`）。裸写 `PARAM_GET` 在两边都不对。

- **失败要能被调用方看见**。读一个没声明过的参数时返回 `success = False`，
  而不是悄悄给个 0 —— 后者会让调用方以为参数存在。

- **参数表就是应用层的一个 dict**。DDS 只负责传输。

## 构建与运行

```zsh
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"
cmake --build build --target ParameterTypesWrapper
```

- 终端 A（先起）：`python3 python/04_parameter/server.py`
- 终端 B：`python3 python/04_parameter/client.py`
- 终端 C（可选）：`python3 python/04_parameter/monitor.py`

预期：客户端打印 declare/get/set/get 四个值（set 之后 get 应该拿到 5），
并有一次"读不存在的参数"失败；终端 C 在参数被修改时打印一条 `[事件]`。

> 注意：连续运行两次 client 时，第二次的 `declare` 会保留第一次设的 5
> （`declare` 用的是 setdefault 语义），所以打印出来不是 1。
