# 第 5 章 · 用 DDS 实现 ROS 2 风格的 action（Python）

本目录与 `cpp/05_action/` 逐行对应，协议说明见那边的 README。

## action 在 DDS 上是什么

四条 Topic，靠 `goal_id` 关联：

```
客户端 --FibonacciGoal------> /demo/fibonacci/goal      --> 服务端
客户端 <--FibonacciFeedback-- /demo/fibonacci/feedback  <-- 服务端
客户端 <--FibonacciResult---- /demo/fibonacci/result    <-- 服务端
客户端 --FibonacciCancel----> /demo/fibonacci/cancel    --> 服务端
```

比服务多出的两件事：**反馈**（服务端单向、多次报进度）和**取消**（客户端随时打断）。
取消单独占一条 Topic，因为它和执行途中其它通信是两回事。

## 图解

### ① 从服务到 action：多了两条通道

```
   ┌─ 第 2 章·服务（2 条通道）───────┐   ┌─ 第 5 章·action（4 条通道）──────────┐
   │                                 │   │                                      │
   │   request  ──request_id──┐      │   │   goal      ──goal_id──┐             │
   │   response ──request_id──┘      │   │   feedback  ──goal_id──┤             │
   │                                 │   │   result    ──goal_id──┤ 靠 id 关联   │
   │   一问一答，完事就结束             │   │   cancel    ──goal_id──┘             │
   │                                 │   │                                      │
   │                                 │   │   feedback 是单向、可多次的            │
   │                                 │   │   cancel   是反向、随时可发的          │
   │                                 │   │   会话在【较长时间内】保持存活           │
   └─────────────────────────────────┘   └──────────────────────────────────────┘
```

四条通道各自的方向和生命周期：

```
                           ┌──────────────────────────────┐
                           │        action 服务端          │
                           │                              │
                           │   handle_goal(handle)：       │
                           │     一边算，一边回头查取消      │
                           └──┬────────┬────────┬────────┬─┘
                              │        │        │        │
      goal   客户端→服务端 ────┘        │        │        └──── cancel  客户端→服务端
      （一次）                          │        │              （零次或一次）
                                        │        │
                     feedback 服务端→客户端        └──── result 服务端→客户端
                     （零次或多次）                        （最后恰好一次）
                              │                                 │
                              ▼                                 ▼
                     客户端边收边显示                        客户端拿到就结束
```

### ② 一次完整执行的时序（正常跑完）

```
  Client                                                   Server
  ──────                                                   ──────
    │ ① send_goal(order=8)                                   │
    │    goal.goal_id = 1                                    │
    │ ────────── /demo/fibonacci/goal ─────────────────────> │ ② goals.take()
    │                                                        │
    │                                                        │ ③ 建 GoalHandle(goal)
    │                                                        │    handle_goal 开始
    │                                                        │
    │ ④ on_feedback() 被回调                                  │    ┌─ 循环 i=0..7 ─┐
    │ <──────── feedback (i=0, val=0) ────────────────────── │    │ ⑤ is_canceling()
    │    → 打印 "反馈 0: 0"                                    │    │    没有 → 继续
    │                                                        │    │ ⑥ publish_feedback(
    │ <──────── feedback (i=1, val=1) ────────────────────── │    │        i, a)
    │    → 打印 "反馈 1: 1"                                    │    │ ⑦ sleep 0.5s
    │                                                        │    └────────────────┘
    │ <──────── feedback (i=2, val=1) ────────────────────── │
    │ <──────── feedback (i=3, val=2) ────────────────────── │
    │ <──────── feedback (i=4, val=3) ────────────────────── │
    │ <──────── feedback (i=5, val=5) ────────────────────── │
    │ <──────── feedback (i=6, val=8) ────────────────────── │
    │ <──────── feedback (i=7, val=13) ───────────────────── │
    │                                                        │ ⑧ 循环结束
    │                                                        │    canceled(False)
    │                                                        │    result(21)
    │ <──────── /demo/fibonacci/result ───────────────────── │ ⑨ results.publish()
    │    goal_id 对得上 → 打印                                │
    │    "结果: 21（已完成）"                                  │
```

**反馈和结果走的是两条不同的 Topic**，客户端要同时盯着两个 reader。
`wait_for_result()` 内部的循环就是「先排空反馈通道 → 再阻塞等一会儿结果」
交替进行的。

### ③ 取消路径的时序（这是和服务的本质区别）

```
  Client                                                   Server
  ──────                                                   ──────
    │ ① send_goal(order=8)                                   │
    │ ─────────────────────────────────────────────────────> │ ② 开始 handle_goal
    │                                                        │
    │ <──────── feedback (i=0, val=0) ────────────────────── │
    │ <──────── feedback (i=1, val=1) ────────────────────── │
    │ <──────── feedback (i=2, val=1) ────────────────────── │
    │                                                        │
    │ ③ 时间超过 1 秒，回调里决定取消：                         │    ┌─ 循环 ─┐
    │    cancel_goal(goal_id=1)                              │    │        │
    │ ──────── /demo/fibonacci/cancel ─────────────────────> │    │ ④ 下一轮
    │                                                        │    │  is_canceling()
    │                     （服务端还在 sleep(0.5) 里）         │    │  ↓
    │                                                        │    │  try_take 到
    │                                                        │    │  cancel.goal_id
    │                                                        │    │      == 1
    │                                                        │    │  → 返回 True
    │                                                        │    │        │
    │                                                        │    │ ⑤ 跳出循环
    │                                                        │    │    canceled(True)
    │                                                        │    │    result(a)
    │                                                        │    └────────┘
    │ <──────── /demo/fibonacci/result ───────────────────── │ ⑥ publish 结果
    │    canceled = True                                     │
    │    → 打印 "结果: 2（已取消）"                            │
```

**第 ④ 步是整章的核心**：服务端在**每一轮循环**都主动回头看一眼取消通道。
因为 `is_canceling()` 用的是非阻塞的 `try_take()`，这一眼不会把执行卡住。

如果误用了阻塞版 `take()`，服务端会卡在"等取消"上 ——
任务既算不完，也永远等不到取消（因为取消只在客户端想取消时才发）。

### ④ goal 的状态机

ROS 2 的 action 有正式的状态机，本章实现的是其中的主干：

```
                         send_goal()
                              │
                              ▼
                         ┌─────────┐
                         │ ACCEPTED│  服务端 take 到目标，建 GoalHandle
                         └────┬────┘
                              │
                              ▼
                         ┌─────────┐
              ┌─────────>│EXECUTING│<─────────┐
              │          └────┬────┘          │
              │               │               │
     publish_feedback()       │               │ 还在跑
     （可发任意多次，           │               │
       状态不变）              │               │
              │               ▼               │
              │        is_canceling() ?       │
              │          │           │        │
              │        是 │           │ 否     │
              │          ▼           └────────┘
              │    ┌───────────┐
              │    │ CANCELING │
              │    └─────┬─────┘
              │          │
              │          ▼
              │    ┌───────────┐
              └────│ CANCELED  │  result.canceled(True)
                   └───────────┘
                            或
                       ┌───────────┐
                       │ SUCCEEDED │  result.canceled(False)
                       └───────────┘
                       （正常跑完）

   本章没实现的（真实 ROS 2 有，教学版略过）：
     ABORTED     —— 执行中出错放弃
     REJECTED    —— 服务端不接受这个目标
```

### ⑤ GoalHandle 的作用：把"执行环境"递给用户代码

```
   ActionServer.spin(handle_goal) 的调用关系：

   ActionServer
     │
     │ ① take 到 goal
     │ ② 构造 GoalHandle(goal, self._feedbacks, self._cancels)
     │         │
     │         ├── 持有 goal 本身           → handle.goal
     │         ├── 持有 feedback 端点        → handle.publish_feedback()
     │         └── 持有 cancel 端点          → handle.is_canceling()
     │
     │ ③ handle_goal(handle)   ← 交给你的回调
     │         │
     │         ▼
     │    ┌─ 你的执行代码 ──────────────────────────────┐
     │    │  for i in range(handle.goal.order()):      │
     │    │      if handle.is_canceling(): ...         │ ← 主动查取消
     │    │      handle.publish_feedback(i, a)         │ ← 主动报进度
     │    └────────────────────────────────────────────┘
     │         │
     │ ④ 拿到返回的 result，回填 goal_id，publish 出去
     │
     ▼
   回到 ① 等下一个目标
```

为什么把两个端点都塞进 handle，而不是让用户直接摸 `ActionServer` 的成员？
—— 因为**执行回调需要的是"跟这个目标相关的那两条通道"**，
而不是整个 server。这也是 `rclpy.action.ServerGoalHandle` 的设计意图。

### ⑥ 服务 vs action：什么时候用哪个

```
   ┌──────────────────┬────────────────────┬──────────────────────┐
   │                  │ 服务（第 2 章）      │ action（本章）        │
   ├──────────────────┼────────────────────┼──────────────────────┤
   │ 通道数            │ 2                  │ 4                    │
   │ 会话长度          │ 一问一答，立刻结束   │ 长时间存活            │
   │ 期间通信          │ 无                 │ 反馈（多次）          │
   │ 中途打断          │ 做不到             │ 取消通道              │
   │ 耗时              │ 毫秒级             │ 秒级到分钟级          │
   │ 适用场景          │ 查询、快速计算      │ 导航、抓取、长任务     │
   └──────────────────┴────────────────────┴──────────────────────┘

   一个经验判断：
     如果调用方需要"知道现在进行到哪了"，或者需要"算了不做了"，
     那就是 action 而不是服务。
```

## 文件说明

| 文件 | 作用 |
|---|---|
| `ActionTypes.idl` | 四个 struct：`FibonacciGoal` / `FibonacciFeedback` / `FibonacciResult` / `FibonacciCancel` |
| `action.py` | 四个类：<br>`Endpoint` —— 最小端点，多了个 `try_take()`（非阻塞取数，**检查取消必须用它**）<br>`GoalHandle` —— 对应 rclpy 的 `ServerGoalHandle`，`publish_feedback()` / `is_canceling()`<br>`ActionServer` —— `spin(execute)`<br>`ActionClient` —— `send_goal()` / `cancel_goal()` / `wait_for_result()` |
| `server.py` | 服务端入口。`handle_goal(handle)` 就是 `execute_callback`：边算边报反馈，每轮查一次取消 |
| `client.py` | 客户端入口。带 `cancel` 参数时会在 1 秒后请求取消 |
| `CMakeLists.txt` | 生成 `ActionTypes.py` + SWIG 模块 |

## 与 rclpy.action 的对应关系

| rclpy.action | 本章 |
|---|---|
| `ActionServer(node, Fibonacci, "fibonacci", cb)` | `ActionServer(root)` + `server.spin(handle_goal)` |
| `ActionClient(node, Fibonacci, "fibonacci")` | `ActionClient(root)` |
| `goal_handle.publish_feedback(fb)` | `handle.publish_feedback(index, value)` |
| `goal_handle.is_canceling()` | `handle.is_canceling()` |
| `client.send_goal_async(goal)` | `client.send_goal(order)` |
| `client.cancel_goal_async(handle)` | `client.cancel_goal(goal_id)` |
| `client.get_result_async(handle)` | `client.wait_for_result(goal_id, on_feedback)` |

## 关键点

**1. 取消检查必须是非阻塞的。** `GoalHandle.is_canceling()` 用的是
`Endpoint.try_take()`（直接调 `take_next_sample()`，不等待），
所以可以安全地放在执行循环的每一轮里。误用阻塞版 `take()` 会让任务
卡在等取消上，永远算不完。

**2. 反馈回调在主线程里被调用。**
`wait_for_result()` 的内部循环是「先排空反馈通道 → 再阻塞等一会儿结果」，
都在调用它的线程上做。所以在反馈回调里直接调 `cancel_goal()` 是安全的，
不需要额外开线程或加锁（`client.py` 就是这么写的）。

**3. 四条通道要全部匹配上**，协议才走得通。`wait_for_clients()` /
`wait_for_server()` 会等全部四条。

**4. 客户端一次只能等一个目标。** `wait_for_result()` 会丢弃 `goal_id`
不匹配的结果。要并发多目标得给每个目标各起一个等待逻辑。

## 构建与运行

```zsh
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"
cmake --build build --target ActionTypesWrapper
```

- 终端 A（先起）：`python3 python/05_action/server.py`
- 终端 B（正常跑完）：`python3 python/05_action/client.py`
- 终端 B（中途取消）：`python3 python/05_action/client.py cancel`

预期输出：客户端先收到 8 条反馈（斐波那契 0,1,1,2,3,5,8,13），
最后结果是 21 并标记"已完成"。加了 `cancel` 时，跑 1 秒后发出取消请求，
服务端在下一轮循环里发现取消、提前返回，结果是当时算到的值并标记"已取消"。
