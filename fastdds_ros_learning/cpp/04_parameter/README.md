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

## 图解

### ① 参数系统 = 服务骨架 + 一条广播通道

这是本章和第 2 章的关系。**斜体的部分是原样照搬服务那一套**：

```
   ┌─ 第 2 章·服务 ─────────────────────┐   ┌─ 第 4 章·参数 ──────────────────┐
   │                                    │   │                                 │
   │  请求 Topic  ──request_id──┐       │   │  请求 Topic  ──request_id──┐    │
   │  响应 Topic  ──request_id──┘       │   │  响应 Topic  ──request_id──┘    │
   │        ↑                           │   │        ↑                        │
   │     靠 id 配对                     │   │     靠 id 配对                  │
   │                                    │   │                                 │
   │                                    │   │  参数事件 Topic  ← 新增！        │
   │                                    │   │    纯广播，没有配对             │
   └────────────────────────────────────┘   └─────────────────────────────────┘

   换句话说：把第 2 章的 Service/Client 复制过来，加一条 ParamEvent 的
   单向 writer，再改改消息字段，就得到了参数系统。
```

三条通道各自的角色：

```
                         ┌─────────────────────────────────────┐
                         │          参数服务端                   │
                         │   ┌───────────────────────────┐     │
                         │   │  参数表（应用层的一个 map）  │     │
                         │   │  "speed" → 5              │     │
                         │   └───────────────────────────┘     │
                         └───┬──────────────┬──────────────┬───┘
                             │              │              │
   /demo/parameters/request  │              │              │  /demo/parameter_events
        （收：只服务端听）     │              │              │   （发：谁都能听）
                             ▼              ▼              ▼
                         ┌────────┐    ┌─────────┐    ┌──────────┐
                         │ 客户端 │    │ 客户端  │    │ 监听进程 │
                         │ A      │    │ B       │    │ (monitor)│
                         └───┬────┘    └────┬────┘    └──────────┘
                             │              │
   /demo/parameters/response │              │
        （发：回给请求方）     │              │
                             └──────────────┘
```

### ② declare / get / set 的完整时序

三个操作走的是**同一条**请求-响应通道，靠 `ParamOp` 枚举区分：

```
  Client                                      Server
  ──────                                      ──────
    │ ① declare_parameter("speed", 1)          │
    │    request.op = PARAM_DECLARE            │
    │    request.name = "speed"                │
    │    request.value = 1                     │
    │ ────────── /demo/parameters/request ───> │ ② switch (op)
    │                                          │      PARAM_DECLARE:
    │                                          │        parameters_.emplace(
    │                                          │            "speed", 1)
    │                                          │        ↑ 已有就保留原值
    │ ④ response.success = true                │
    │    response.value = 1                    │ ③ 回响应（不发事件）
    │ <──────── /demo/parameters/response ──── │
    │    → 打印 "declare speed = 1"             │
    │                                          │
    │ ⑤ get_parameter("speed")                 │
    │    request.op = PARAM_GET                │
    │ ───────────────────────────────────────> │ ⑥ PARAM_GET:
    │                                          │      查 map，"speed" → 1
    │ <──────── response.value = 1 ─────────── │ ⑦ 回响应（不event）
    │    → 打印 "get speed = 1"                 │
    │                                          │
    │ ⑧ set_parameter("speed", 5)              │
    │    request.op = PARAM_SET                │
    │    request.value = 5                     │
    │ ───────────────────────────────────────> │ ⑨ PARAM_SET:
    │                                          │      parameters_["speed"] = 5
    │                                          │
    │                                          │ ⑩ ✓ 额外广播事件！
    │                                          │ ──── /demo/parameter_events ──>
    │ ⑪ response.value = 5                     │         （monitor 收到）
    │ <──────── response ───────────────────── │ ⑫ 回响应
    │                                          │
    │ ⑬ get_parameter("speed")                 │
    │ ───────────────────────────────────────> │ ⑭ 查 map → 5
    │ <──────── response.value = 5 ─────────── │
    │    → 打印 "get speed = 5"                 │
    │                                          │
    │ ⑮ get_parameter("no_such_param")         │
    │ ───────────────────────────────────────> │ ⑯ 查不到
    │ <─── response.success = false ─────────── │    如实返回失败，
    │    → 打印失败（预期行为）                    │    不悄悄给个 0
```

**只有 ⑩ 这一步是"多出来的"** —— 其它都和第 2 章的服务一模一样。

### ③ 为什么事件要单独一条 Topic

如果不单独开，把事件塞进响应里行不行？

```
   ✗ 塞进响应里的问题：

     参数变化 ──> 响应只回给【发起修改的那个客户端】

        客户端 A ──set──> 服务端
                            │
                            └──response──> 客户端 A    ← A 知道了
                                           （B、C 什么都没收到）
        客户端 B、C ──→ 完全不知道 speed 变了 ⚠

   ✓ 单独开一条广播 Topic：

     参数变化 ──> 服务端往一条【独立的】Topic 上写一次

        客户端 A ──set──> 服务端
                            ├──response──> 客户端 A    ← 谁问的给谁
                            │
                            └──ParamEvent─> /demo/parameter_events
                                              │
                                              ├──> 监听进程 monitor
                                              ├──> 客户端 B（如果订了）
                                              └──> 客户端 C（如果订了）

        ↑ 服务端只写一次，DDS 负责复制给所有订阅者 —— 这就是"一对多"
```

这正是 ROS 2 里 `parameter_events` 话题存在的理由：**参数变化是需要
被广播出去的全局事件，不是某个请求的私有响应**。两者的语义完全不同，
所以必须在 DDS 上占两条不同的 Topic。

### ④ IDL enum 在两端的写法差异

`ParamOp` 是本章新引入的东西。同**一份** IDL，生成到两端却长得不一样：

```
   ParameterTypes.idl
   ──────────────────
   enum ParamOp
   {
       PARAM_DECLARE,
       PARAM_GET,
       PARAM_SET
   };
              │
              ├──────────────────────────────┬──────────────────────────────┐
              ▼                              ▼
   ┌─ C++ 生成 ─────────────────┐   ┌─ Python 生成 ────────────────────────┐
   │                            │   │                                       │
   │  enum class ParamOp        │   │  # SWIG 把枚举类型名当前缀拼进去        │
   │      : int32_t             │   │  ParamOp_PARAM_DECLARE = 0            │
   │  {                         │   │  ParamOp_PARAM_GET     = 1            │
   │      PARAM_DECLARE,        │   │  ParamOp_PARAM_SET     = 2            │
   │      PARAM_GET,            │   │                                       │
   │      PARAM_SET             │   │  # 平铺在模块命名空间下                │
   │  };                        │   │  ParameterTypes.ParamOp_PARAM_GET     │
   │                            │   │                                       │
   │  // 有作用域，必须带前缀：    │   │  # 没有作用域概念，直接当常量用         │
   │  ParamOp::PARAM_GET        │   │  ParameterTypes.ParamOp_PARAM_GET     │
   └────────────────────────────┘   └───────────────────────────────────────┘

   ⚠ 裸写 PARAM_GET：C++ 编译不过；Python 会 NameError。
     这类"同一份 IDL、两端写法不同"的地方，是跨语言调试最容易卡住的点。
```

**为什么用 enum 而不是 0/1/2**：如果 `op` 是 `long`，代码里就会出现
`if (request.op() == 1)` 这种靠注释才能读懂的判断。用 enum 之后
`case ParamOp::PARAM_GET:` 自解释，而且两端都有编译期/运行期的名字检查。

### ⑤ 参数表在哪里

```
   ┌─ 服务端进程 ──────────────────────────────────────────────┐
   │                                                          │
   │   std::map<std::string, long> parameters_;   ← 【就是它】  │
   │        "speed" → 5                                       │
   │                                                          │
   │   这是普通的应用层数据结构，和 DDS 没有任何关系。            │
   │   DDS 只负责把 ParamRequest / ParamResponse / ParamEvent   │
   │   在进程之间搬来搬去。                                     │
   └──────────────────────────────────────────────────────────┘
                              ▲
                              │
        DDS 提供的只是"传输"这一层：
                              │
   ┌──────────────────────────┴───────────────────────────────┐
   │  序列化 → RTPS → 网络 → 反序列化 → 放进对端缓存             │
   └──────────────────────────────────────────────────────────┘

   由此推出一个重要结论：
     参数的【语义】全在你自己的代码里 —— 什么算"已声明"、
     读未声明的参数该怎么办、能不能改、改了要不要通知，
     都是你定义的。ROS 2 规定了这些语义，本章只是模仿它。
```

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
