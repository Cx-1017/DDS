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

## 文件说明

| 文件 | 作用 |
|---|---|
| `ServiceTypes.idl` | 两个类型：`AddRequest{request_id, a, b}` 和 `AddResponse{request_id, sum}`。对应 ROS 2 `.srv` 文件里 `---` 分隔的两段 |
| `service.hpp` | 本章唯一的头文件，三层结构：<br>① `Endpoint<Sample, PubSubType>` —— 最小话题端点，**全章唯一直接碰 DDS 的地方**，构造函数里逐行注释了每一步对应哪个实体<br>② `Service` —— 服务端，`spin()` 对应 `rclcpp::spin`<br>③ `Client` —— 客户端，`call()` 对应 `async_send_request` |
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
