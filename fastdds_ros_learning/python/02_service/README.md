# 第 2 章 · 用 DDS 实现 ROS 2 风格的服务（Python）

本目录与 `cpp/02_service/` 逐行对应，协议说明见那边的 README，
这里只讲 Python 侧的文件和差异。

## 服务在 DDS 上是什么

DDS 没有 RPC，服务的"请求-响应"语义要用两条 Topic 拼：

```
客户端 --AddRequest-->  /demo/add/request  --> 服务端
客户端 <--AddResponse-- /demo/add/response <-- 服务端
```

两条 Topic 靠 `request_id` 关联 —— 响应把请求的 id 原样带回来，
客户端只认 id 对得上的那条响应。

## 文件说明

| 文件 | 作用 |
|---|---|
| `ServiceTypes.idl` | `AddRequest{request_id, a, b}` 和 `AddResponse{request_id, sum}`，与 cpp 侧内容相同 |
| `service.py` | 三层结构：<br>① `Endpoint` —— 最小话题端点，**全章唯一直接碰 DDS 的地方**，构造函数逐行注释<br>② `Service` —— 服务端，`spin(handler)` 对应 `rclpy.spin`<br>③ `Client` —— 客户端，`call(a, b)` 对应 `call_async`（这里写成同步的，为了把配对过程讲清楚） |
| `server.py` | 服务端入口。`handle_add(request)` 就是服务提供的能力 |
| `client.py` | 客户端入口 |
| `CMakeLists.txt` | 生成 `ServiceTypes.py` + SWIG 模块 |

## 与 rclpy 的对应关系

| rclpy | 本章 |
|---|---|
| `node.create_service(Add, "add", handler)` | `Service(req_topic, resp_topic)` + `service.spin(handler)` |
| `node.create_client(Add, "add")` | `Client(req_topic, resp_topic)` |
| `client.wait_for_service()` | `client.wait_for_service()` |
| `client.call_async(request)` → future | `client.call(a, b)`（同步版） |
| `rclpy.spin(node)` | `service.spin(handler)` |

## Python 侧的关键点

- **`wait_for_unread_message()` 返回 `bool`**（有数据 `True` / 超时 `False`），
  不是 `ReturnCode_t`。别写成 `!= fastdds.RETCODE_OK`。
- **`Duration_t` 只暴露了双参数构造** `(秒, 纳秒)`。不能把总秒数乘 `1e9`
  当纳秒传进去：5 秒 = 5e9 纳秒超过了 `uint32` 上限，Python 绑定会直接抛
  `TypeError`（C++ 那边是静默溢出，更隐蔽）。`to_duration()` 负责拆分。
- **所有 SWIG 对象都要保持引用**（`Endpoint` 把实体都存成实例属性）。
  被 GC 回收的实体可能已经注册进 DDS，会出现很难查的「对象还在但收不到数据」。

## 构建与运行

```zsh
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"
cmake --build build --target ServiceTypesWrapper
```

- 终端 A（先起）：`python3 python/02_service/server.py`
- 终端 B：`python3 python/02_service/client.py`

预期：客户端打印 `add(20, 22) = 42`，服务端打印收到的请求。

两端的类型是二进制兼容的 —— 可以拿 C++ 的 `service_client` 去调 Python 的
`server.py`（或反过来），这是验证「两份 IDL 副本」策略是否正确的直接方法。
