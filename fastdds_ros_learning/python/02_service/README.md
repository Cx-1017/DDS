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

## 图解

### ① ROS 的服务是怎么"降级"成 DDS 两条 Topic 的

ROS 2 的服务 API 看起来是一次函数调用，但在 DDS 这一层根本没有"调用"
这个东西：

```
   ROS 2 眼里的世界（一次调用）           DDS 眼里的世界（两条独立的数据流）
   ────────────────────────────          ──────────────────────────────────

   client.call_async(req)                ①客户端 write AddRequest
        │                                    到 /demo/add/request
        │                                        │
        │                                        ▼  （发布订阅，一对多通道）
        │                                   ②服务端 take 到请求
        │                                        │
        │   （中间这段时间客户端在等）             │  服务端算 a + b
        │                                        │
        │                                   ③服务端 write AddResponse
        │                                    到 /demo/add/response
        │                                        │
        ▼                                        ▼  （又一条独立通道）
   拿到 future 的结果                     ④客户端 take 到响应
                                              找 request_id 对得上的那条

   ↑ 一次"调用"，                     ↑ 两段互不相干的单向数据流，
     是个假象                             中间没有任何"连接"存在
```

**关键认知**：DDS 不会替你记住"这次响应是给谁的"。两条 Topic 之间
唯一的联系，就是你在应用层自己塞进消息里的 `request_id`。

### ② 一次 call() 的完整时序

```
   Client                                       Server
   ──────                                       ──────
     │                                            │
     │ ① client.wait_for_service()                │
     │    轮询两条 Topic 的匹配数                   │  ← 服务端要先起
     │ <──────────── 发现完成 ────────────────────> │
     │                                            │
     │ ② request.request_id(1)                    │
     │    request.a(20); request.b(22)            │
     │                                            │
     │ ③ self._requests.publish(request)          │
     │ ─────────── /demo/add/request ───────────> │
     │                                            │ ④ requests.take()
     │                                            │    收到请求 #1
     │                                            │
     │                                            │ ⑤ handler(request)
     │                                            │    sum = 20 + 22 = 42
     │                                            │
     │                                            │ ⑥ response.request_id(
     │                                            │       request.request_id())
     │                                            │    ⚠ 原样带回去，这是配对的全部
     │                                            │
     │ ⑧ responses.take()                         │ ⑦ responses.publish(response)
     │    检查 request_id == 1 ？                 │
     │ ─────────── /demo/add/response ──────────< │
     │    对得上 → 返回                            │
     │                                            │
   调用结束                                     回到 ④ 等下一个请求
```

第 ⑥ 步是整章的核心：**服务端唯一的义务就是"把 id 原样带回"**。

### ③ 为什么非要 request_id —— 没有它会怎样

```
   没有 request_id 的情况：

   Client                                    Server
     │                                          │
     │ ──── request(a=1, b=2) ────────────────> │  收到①
     │ ──── request(a=10, b=20) ──────────────> │  收到②
     │ <──────────────── response(sum=3) ────── │  算完①发出
     │ <──────────────── response(sum=30) ───── │  算完②发出
     │                                          │
     │ 客户端拿到第一条 sum=3                       │
     │  → 它是哪个请求的响应？并发时两个线程可能各自    │
     │    拿到对方的结果 ⚠                          │

   加了 request_id 之后：

     │ ──── request(id=1, a=1, b=2) ──────────> │
     │ ──── request(id=2, a=10, b=20) ────────> │
     │ <──────── response(id=2, sum=30) ─────── │  ← 先回来的可能是 ②
     │ <──────── response(id=1, sum=3) ──────── │
     │                                          │
     │ 客户端：id=2 → 交给等 id=2 的逻辑            │
     │         id=1 → 交给等 id=1 的逻辑            │
     │         id 不认识 → 丢掉（不是给我的）         │
```

**DDS 只保证数据能送到，不保证顺序、不保证对应关系。** 这两件事都得
应用层自己做 —— 这正是 ROS 2 的 RMW 层替你干掉的活。

### ④ 每个 Endpoint 在 DDS 里建了什么

`Endpoint` 每实例化一次，就在参与者下面建一整条链路。`Service` 有两个
Endpoint，`Client` 也有两个，方向相反：

```
   Service（服务端）                        Client（客户端）
   ┌─ DomainParticipant ─────────┐         ┌─ DomainParticipant ─────────┐
   │  注册类型 AddRequest         │         │  注册类型 AddRequest         │
   │  Topic /demo/add/request    │         │  Topic /demo/add/request    │
   │    Subscriber → DataReader  │ ←收     │    Publisher → DataWriter 发→ │
   │                             │         │                             │
   │  注册类型 AddResponse        │         │  注册类型 AddResponse        │
   │  Topic /demo/add/response   │         │  Topic /demo/add/response   │
   │    Publisher → DataWriter 发→│         │    Subscriber → DataReader ←收│
   └─────────────────────────────┘         └─────────────────────────────┘
              │                                        │
              └────────── 同一个 Topic 名 + 同一个类型名 ──────────┘
                          → 发现协议自动把它们配对
```

两端**都在自己的参与者上注册了同一个类型**（`AddRequest` 各注册一次）。
这没问题，因为它们是两个不同的参与者 —— 同类型名重复注册只在**同一个**
参与者内才算错误。

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
