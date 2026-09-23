# 第 2 章 · 用 DDS 实现 ROS 2 风格的服务

## 服务在 DDS 上是什么

DDS 只有发布订阅，**没有 RPC**。ROS 2 的服务是"请求-响应"语义，
要用两条 Topic 拼出来：

```
客户端 --AddRequest-->  /demo/add/request  --> 服务端
客户端 <--AddResponse-- /demo/add/response <-- 服务端
```

两条 Topic 靠 `request_id` 关联：响应把请求的 id 原样带回来，
客户端只认 id 对得上的那条响应 —— 这样多个请求并发也不会串味。
ROS 2 的真实服务由 RMW 按 sequence number 自动完成这个配对，
这里显式写出来是为了看清"RPC 在 DDS 上到底是怎么拼的"。

## 图解

### ① ROS 的服务是怎么"降级"成 DDS 两条 Topic 的

ROS 2 的服务 API 看起来是一次函数调用，但在 DDS 这一层根本没有"调用"
这个东西。整个映射关系是这样的：

```
   ROS 2 眼里的世界（一次调用）           DDS 眼里的世界（两条独立的数据流）
   ────────────────────────────          ──────────────────────────────────

   client->call(req)                     ①客户端 write AddRequest
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
   拿到 response                          ④客户端 take 到响应
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
     │    轮询 request/response 两条 Topic 的匹配数 │  ← 服务端要先起
     │ <──────────── 发现完成 ────────────────────> │
     │                                            │
     │ ② request.request_id(1)                    │
     │    request.a(20); request.b(22)            │
     │                                            │
     │ ③ requests_.publish(request)               │
     │ ─────────── /demo/add/request ───────────> │
     │                                            │ ④ requests_.take()
     │                                            │    收到请求 #1
     │                                            │
     │                                            │ ⑤ handler(request)
     │                                            │    sum = 20 + 22 = 42
     │                                            │
     │                                            │ ⑥ response.request_id(
     │                                            │       request.request_id())
     │                                            │    ⚠ 原样带回去，这是配对的全部
     │                                            │
     │ ⑧ responses_.take()                        │ ⑦ responses_.publish(response)
     │    检查 request_id == 1 ？                 │
     │ ─────────── /demo/add/response ──────────< │
     │    对得上 → 返回                            │
     │                                            │
   调用结束                                     回到 ④ 等下一个请求
```

第 ⑥ 步是整章的核心：**服务端唯一的义务就是"把 id 原样带回"**。

### ③ 为什么非要 request_id —— 没有它会怎样

假设去掉 `request_id`，客户端发两个请求、服务端依次响应：

```
   没有 request_id 的情况：

   Client                                    Server
     │                                          │
     │ ──── request(a=1, b=2) ────────────────> │  收到①
     │ ──── request(a=10, b=20) ──────────────> │  收到②
     │                                          │  算 ① 得 3
     │ <──────────────── response(sum=3) ────── │  发出
     │                                          │  算 ② 得 30
     │ <──────────────── response(sum=30) ───── │  发出
     │                                          │
     │ 客户端拿到第一条 sum=3                       │
     │  → 它以为是哪个请求的响应？                  │
     │    如果客户端是"发完就等一条"，                  │
     │    两个并发线程可能各自拿到对方的响应 ⚠         │

   加了 request_id 之后：

     │ ──── request(id=1, a=1, b=2) ──────────> │
     │ ──── request(id=2, a=10, b=20) ────────> │
     │ <──────── response(id=2, sum=30) ─────── │  ← 先回来的可能是 ②
     │ <──────── response(id=1, sum=3) ──────── │
     │                                          │
     │ 客户端：id=2 → 交给等 id=2 的那个逻辑        │
     │         id=1 → 交给等 id=1 的那个逻辑        │
     │         id 不认识 → 丢掉（不是给我的）        │
```

**DDS 只保证数据能送到，不保证顺序、不保证对应关系。** 这两件事都得
应用层自己做 —— 这正是 ROS 2 的 RMW 层替你干掉的活。

### ④ 每个 Endpoint 在 DDS 里建了什么

本章的 `Endpoint<>` 模板每实例化一次，就在参与者下面建一整条链路。
`Service` 有两个 Endpoint，`Client` 也有两个，方向相反：

```
   Service（服务端）                        Client（客户端）
   ┌─ DomainParticipant ────────┐          ┌─ DomainParticipant ────────┐
   │                            │          │                            │
   │  Endpoint<AddRequest>      │          │  Endpoint<AddRequest>      │
   │    register_type(AddRequest)│          │    register_type(AddRequest)│
   │    Topic /demo/add/request │          │    Topic /demo/add/request │
   │    Subscriber              │          │    Publisher               │
   │      └── DataReader   ←收  │          │      └── DataWriter   发→  │
   │                            │          │                            │
   │  Endpoint<AddResponse>     │          │  Endpoint<AddResponse>     │
   │    register_type(AddResponse)         │    register_type(AddResponse)
   │    Topic /demo/add/response│          │    Topic /demo/add/response│
   │    Publisher               │          │    Subscriber              │
   │      └── DataWriter   发→  │          │      └── DataReader   ←收  │
   └────────────────────────────┘          └────────────────────────────┘
              │                                        │
              └────────── 同一个 Topic 名 + 同一个类型名 ──────────┘
                          → 发现协议自动把它们配对
```

注意两端**都在同一个参与者上注册了同一个类型**（`AddRequest` 各注册一次）。
这没问题，因为它们是两个不同的参与者 —— 同类型名重复注册只在**同一个**
参与者内才算错误。

## 文件说明

| 文件 | 作用 |
|---|---|
| `ServiceTypes.idl` | 两个类型：`AddRequest{request_id, a, b}` 和 `AddResponse{request_id, sum}`。对应 ROS 2 `.srv` 文件里 `---` 分隔的两段 |
| `service.hpp` | 本章唯一的头文件，三层结构：① `Endpoint<Sample, PubSubType>`（最小话题端点，**全章唯一直接碰 DDS 的地方**，构造函数里逐行注释了每一步对应哪个实体）；② `Service`（服务端，`spin()` 对应 `rclcpp::spin`）；③ `Client`（客户端，`call()` 对应 `async_send_request`） |
| `server.cpp` | 服务端入口。只写业务逻辑（`handler` 里做加法），DDS 细节都在 `.hpp` 里 |
| `client.cpp` | 客户端入口。`wait_for_service()` → `call(20, 22)` |
| `CMakeLists.txt` | 本章构建 |

## 与 rclcpp 的对应关系

| rclcpp | 本章 |
|---|---|
| `node->create_service<Add>("add", handler)` | `Service service(req_topic, resp_topic)` + `service.spin(handler)` |
| `node->create_client<Add>("add")` | `Client client(req_topic, resp_topic)` |
| `client->wait_for_service()` | `client.wait_for_service()` |
| `client->async_send_request(req)` → `future` | `client.call(a, b, out)`（同步版，为了把配对过程讲清楚） |
| `rclcpp::spin(node)` | `service.spin(handler)` |

## 关键点

- **`request_id` 是全部**。DDS 不帮你关联请求和响应，id 是应用层自己加的约定。
  真实 ROS 2 服务里这个字段由 RMW 生成和管理。
- **两端都要先等发现完成再发**。DDS 靠后台的发现协议找对端，这需要时间；
  不等就发，样本会在对端还没匹配上时被直接丢掉，客户端只能等到超时。
  这是 DDS 上做 RPC 最常见的坑。
- **`wait_for_unread_message()` 返回 `bool` 不是 `ReturnCode_t`**，
  别写成 `RETCODE_OK != ...` —— 那样会因为 `0 != true` 恒成立而永远跳过数据。
- **`Duration_t` 是（int32 秒, uint32 纳秒）两个字段**，不能把毫秒乘 1000000
  直接塞第二个参数：5000ms = 5e9 纳秒超过了 `uint32` 上限会静默溢出。
  `to_duration()` 用的是 `Duration_t` 那个接受 `long double` 秒的构造函数。

## 构建与运行

```zsh
cmake --build build --target service_server service_client
```

- 终端 A（先起）：`./build/cpp/02_service/service_server`
- 终端 B：`./build/cpp/02_service/service_client`

预期输出：客户端打印 `add(20, 22) = 42`，服务端打印收到的请求。

Python 版见 `python/02_service/`。两端的类型是二进制兼容的，
可以拿 C++ 的 client 去调 Python 的 server（或反过来），验证这一点。
