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

## 图解

### ① 参数系统 = 服务骨架 + 一条广播通道

这是本章和第 2 章的关系：

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
                         │   │  参数表（Python 的一个 dict）│     │
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
    │ ────────── /demo/parameters/request ───> │ ② PARAM_DECLARE:
    │                                          │      setdefault("speed", 1)
    │ ④ response.success = true                │        ↑ 已有就保留原值
    │    response.value = 1                    │ ③ 回响应（不发事件）
    │ <──────── /demo/parameters/response ──── │
    │    → 打印 "declare speed = 1"             │
    │                                          │
    │ ⑤ get_parameter("speed")                 │
    │ ───────────────────────────────────────> │ ⑥ 查 dict → 1
    │ <──────── response.value = 1 ─────────── │ ⑦ 回响应（不发事件）
    │    → 打印 "get speed = 1"                 │
    │                                          │
    │ ⑧ set_parameter("speed", 5)              │
    │ ───────────────────────────────────────> │ ⑨ dict["speed"] = 5
    │                                          │
    │                                          │ ⑩ ✓ 额外广播事件！
    │                                          │ ──── /demo/parameter_events ──>
    │ ⑪ response.value = 5                     │         （monitor 收到）
    │ <──────── response ───────────────────── │ ⑫ 回响应
    │                                          │
    │ ⑬ get_parameter("speed")                 │
    │ ───────────────────────────────────────> │ ⑭ 查 dict → 5
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

```
   ✗ 塞进响应里的问题：

     参数变化 ──> 响应只回给【发起修改的那个客户端】

        客户端 A ──set──> 服务端
                            │
                            └──response──> 客户端 A    ← A 知道了
                                           （B、C 什么都没收到）
        客户端 B、C ──→ 完全不知道 speed 变了 ⚠

   ✓ 单独开一条广播 Topic：

        客户端 A ──set──> 服务端
                            ├──response──> 客户端 A    ← 谁问的给谁
                            │
                            └──ParamEvent─> /demo/parameter_events
                                              ├──> 监听进程 monitor
                                              ├──> 客户端 B（如果订了）
                                              └──> 客户端 C（如果订了）

        ↑ 服务端只写一次，DDS 负责复制给所有订阅者 —— 这就是"一对多"
```

这正是 ROS 2 里 `parameter_events` 话题存在的理由：**参数变化是需要
被广播出去的全局事件，不是某个请求的私有响应**。

### ④ IDL enum 在两端的写法差异

同**一份** IDL，生成到两端却长得不一样：

```
   ParameterTypes.idl
   ──────────────────
   enum ParamOp { PARAM_DECLARE, PARAM_GET, PARAM_SET };
              │
              ├──────────────────────────────┬──────────────────────────────┐
              ▼                              ▼
   ┌─ C++ 生成 ─────────────────┐   ┌─ Python 生成 ────────────────────────┐
   │  enum class ParamOp        │   │  # SWIG 把枚举类型名当前缀拼进去        │
   │      : int32_t             │   │  ParamOp_PARAM_DECLARE = 0            │
   │  {                         │   │  ParamOp_PARAM_GET     = 1            │
   │      PARAM_DECLARE,        │   │  ParamOp_PARAM_SET     = 2            │
   │      PARAM_GET,            │   │                                       │
   │      PARAM_SET             │   │  # 平铺在模块命名空间下，没有作用域      │
   │  };                        │   │  ParameterTypes.ParamOp_PARAM_GET     │
   │                            │   │                                       │
   │  // 有作用域，必须带前缀：    │   │                                       │
   │  ParamOp::PARAM_GET        │   │                                       │
   └────────────────────────────┘   └───────────────────────────────────────┘

   ⚠ 裸写 PARAM_GET：C++ 编译不过；Python 会 NameError。
```

### ⑤ 参数表在哪里

```
   ┌─ 服务端进程 ──────────────────────────────────────────────┐
   │                                                          │
   │   self._parameters = {}        ← 【就是它】一个普通 dict   │
   │        {"speed": 5}                                      │
   │                                                          │
   │   和 DDS 没有任何关系。                                    │
   │   DDS 只负责把 ParamRequest / ParamResponse / ParamEvent   │
   │   在进程之间搬来搬去。                                     │
   └──────────────────────────────────────────────────────────┘
                              ▲
        DDS 提供的只是"传输"这一层：
                              │
   ┌──────────────────────────┴───────────────────────────────┐
   │  序列化 → RTPS → 网络 → 反序列化 → 放进对端缓存             │
   └──────────────────────────────────────────────────────────┘

   参数的【语义】全在你自己的代码里 —— 什么算"已声明"、
   读未声明的参数该怎么办、改了要不要通知，都是你定义的。
```

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
