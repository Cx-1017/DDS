# 第 5 章 · 用 DDS 实现 ROS 2 风格的 action

## action 在 DDS 上是什么

action 是**有状态的长任务**协议。它比服务多出两件事：

| | 服务（第 2 章） | action（本章） |
|---|---|---|
| 会话长度 | 一次请求一个响应就结束 | 长时间存活，期间持续通信 |
| 反馈 | 无 | 服务端单向、多次地报进度 |
| 取消 | 无 | 客户端随时可以打断执行 |

对应到 DDS 是**四条 Topic**，靠 `goal_id` 关联：

```
客户端 --FibonacciGoal------> /demo/fibonacci/goal      --> 服务端
客户端 <--FibonacciFeedback-- /demo/fibonacci/feedback  <-- 服务端
客户端 <--FibonacciResult---- /demo/fibonacci/result    <-- 服务端
客户端 --FibonacciCancel----> /demo/fibonacci/cancel    --> 服务端
```

取消为什么单独一条 Topic？因为它是**执行途中**发过来的，和"发目标"是两件不同的事，
混在一条 Topic 上反而要加状态字段去区分。

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
                           │   execute_callback：          │
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
    │                                                        │
    │ ① send_goal(order=8)                                   │
    │    goal.goal_id = 1                                    │
    │ ────────── /demo/fibonacci/goal ─────────────────────> │ ② goals_.take()
    │                                                        │
    │                                                        │ ③ 建 GoalHandle(goal)
    │                                                        │    execute(handle) 开始
    │                                                        │
    │ ④ on_feedback() 被回调                                  │    ┌─ 循环 i=0..7 ─┐
    │ <──────── /demo/fibonacci/feedback (i=0, val=0) ─────── │    │ ⑤ 查 is_canceling()
    │    → 打印 "反馈 0: 0"                                    │    │    没有 → 继续
    │                                                        │    │ ⑥ publish_feedback(
    │ <──────── feedback (i=1, val=1) ────────────────────── │    │        i, a)
    │    → 打印 "反馈 1: 1"                                    │    │ ⑦ sleep 500ms
    │                                                        │    └────────────────┘
    │ <──────── feedback (i=2, val=1) ────────────────────── │
    │ <──────── feedback (i=3, val=2) ────────────────────── │
    │ <──────── feedback (i=4, val=3) ────────────────────── │
    │ <──────── feedback (i=5, val=5) ────────────────────── │
    │ <──────── feedback (i=6, val=8) ────────────────────── │
    │ <──────── feedback (i=7, val=13) ───────────────────── │
    │                                                        │ ⑧ 循环结束
    │                                                        │    result.canceled(false)
    │                                                        │    result.result(21)
    │ <──────── /demo/fibonacci/result ───────────────────── │ ⑨ results_.publish()
    │    goal_id 对得上 → 打印                                │
    │    "结果: 21（已完成）"                                  │
    │                                                        │
```

**注意反馈和结果走的是两条不同的 Topic**，客户端要同时盯着两个 reader。
`wait_for_result()` 内部的循环就是「先排空反馈通道 → 再阻塞等一会儿结果」
交替进行的。

### ③ 取消路径的时序（这是和服务的本质区别）

```
  Client                                                   Server
  ──────                                                   ──────
    │ ① send_goal(order=8)                                   │
    │ ─────────────────────────────────────────────────────> │ ② 开始 execute()
    │                                                        │
    │ <──────── feedback (i=0, val=0) ────────────────────── │
    │ <──────── feedback (i=1, val=1) ────────────────────── │
    │ <──────── feedback (i=2, val=1) ────────────────────── │
    │                                                        │
    │ ③ 时间超过 1 秒，回调里决定取消：                         │    ┌─ 循环 ─┐
    │    cancel_goal(goal_id=1)                              │    │        │
    │ ──────── /demo/fibonacci/cancel ─────────────────────> │    │ ④ 下一轮
    │                                                        │    │  is_canceling()
    │                     （服务端还在 sleep 500ms 里）        │    │  ↓
    │                                                        │    │  try_take 到
    │                                                        │    │  cancel.goal_id
    │                                                        │    │      == 1
    │                                                        │    │  → 返回 true
    │                                                        │    │        │
    │                                                        │    │ ⑤ 跳出循环
    │                                                        │    │    result.canceled(
    │                                                        │    │        true)
    │                                                        │    │    result.result(a)
    │                                                        │    └────────┘
    │ <──────── /demo/fibonacci/result ───────────────────── │ ⑥ publish 结果
    │    canceled = true                                     │
    │    → 打印 "结果: 2（已取消）"                            │
    │                                                        │
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
              └────│ CANCELED  │  result.canceled(true)
                   └───────────┘
                            或
                       ┌───────────┐
                       │ SUCCEEDED │  result.canceled(false)
                       └───────────┘
                       （正常跑完）

   本章没实现的（真实 ROS 2 有，教学版略过）：
     ABORTED     —— 执行中出错放弃
     REJECTED    —— 服务端不接受这个目标
     CANCELING 到 CANCELED 之间服务端"怎么"停，由你决定
```

### ⑤ GoalHandle 的作用：把"执行环境"递给用户代码

```
   ActionServer::spin(execute) 的调用关系：

   ActionServer
     │
     │ ① take 到 goal
     │ ② 构造 GoalHandle(goal, &feedbacks_, &cancels_)
     │         │
     │         ├── 持有 goal 本身
     │         ├── 持有 feedback 端点的指针（用来发反馈）
     │         └── 持有 cancel 端点的指针（用来查取消）
     │
     │ ③ execute(handle)   ← 交给你的回调
     │         │
     │         ▼
     │    ┌─ 你的执行代码 ────────────────────────────┐
     │    │  for (i = 0; i < handle.goal().order(); ++i) {
     │    │      if (handle.is_canceling()) { ... }   │ ← 主动查取消
     │    │      handle.publish_feedback(i, a);       │ ← 主动报进度
     │    │  }                                        │
     │    └──────────────────────────────────────────┘
     │         │
     │ ④ 拿到返回的 result，回填 goal_id，publish 出去
     │
     ▼
   回到 ① 等下一个目标
```

为什么把两个端点都塞进 handle，而不是让用户直接拿 ActionServer 的成员？
—— 因为**执行回调需要的是"跟这个目标相关的那两条通道"**，
而不是整个 server。这也是 rclcpp_action 里 `ServerGoalHandle` 的设计意图。

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
| `ActionTypes.idl` | 四个 struct：`FibonacciGoal` / `FibonacciFeedback` / `FibonacciResult` / `FibonacciCancel`。对应 ROS 2 `.action` 文件里 `---` 分隔的三段，外加一条取消通道 |
| `action.hpp` | 四个类：<br>`Endpoint<>` —— 最小端点，多了个 `try_take()`（非阻塞取数，**检查取消必须用它**）<br>`GoalHandle` —— 对应 rclcpp_action 的 `ServerGoalHandle`，`publish_feedback()` / `is_canceling()`<br>`ActionServer` —— `spin(execute)`<br>`ActionClient` —— `send_goal()` / `cancel_goal()` / `wait_for_result()` |
| `server.cpp` | 服务端入口。`spin()` 里的 lambda 就是 rclcpp_action 的 `execute_callback`：边算边报反馈，每轮查一次取消 |
| `client.cpp` | 客户端入口。带 `cancel` 参数时会在 1 秒后请求取消 |
| `CMakeLists.txt` | 本章构建 |

## 与 rclcpp_action 的对应关系

| rclcpp_action | 本章 |
|---|---|
| `create_server<Fibonacci>(node, "fibonacci", ...)` | `ActionServer server(root)` + `server.spin(execute)` |
| `create_client<Fibonacci>(node, "fibonacci")` | `ActionClient client(root)` |
| `goal_handle->publish_feedback(fb)` | `handle.publish_feedback(index, value)` |
| `goal_handle->is_canceling()` | `handle.is_canceling()` |
| `client->async_send_goal(req)` | `client.send_goal(order)` |
| `client->async_cancel_goal(goal)` | `client.cancel_goal(goal_id)` |
| `client->async_get_result(goal)` | `client.wait_for_result(goal_id, out, on_feedback)` |

## 关键点

**1. 取消检查必须是非阻塞的。** `GoalHandle::is_canceling()` 内部用的是
`Endpoint::try_take()`（直接调 `take_next_sample()`，不等待），
所以它可以安全地放在执行循环的**每一轮**里调用。
如果误用阻塞版 `take()`，整个任务就会卡在等取消上，永远算不完。

**2. 四条通道要全部匹配上，协议才走得通。**
`wait_for_clients()` / `wait_for_server()` 会等全部四条，缺一条都会导致行为异常。

**3. 反馈回调在主线程里被调用。** `wait_for_result()` 的内部循环是
「先排空反馈通道 → 再阻塞等一会儿结果」，两件事都在调用它的那个线程上做。
所以在反馈回调里直接调 `cancel_goal()` 是安全的，不需要额外开线程或加锁。

**4. 客户端一次只能等一个目标。** `wait_for_result()` 会丢弃 `goal_id` 不匹配的
结果，本示例一次只跑一个目标。要并发多目标，得给每个目标各起一个等待逻辑 ——
ROS 2 的 action 客户端就是用 goal handle 列表来管理这件事的。

## 构建与运行

```zsh
cmake --build build --target action_server action_client
```

- 终端 A（先起）：`./build/cpp/05_action/action_server`
- 终端 B（正常跑完）：`./build/cpp/05_action/action_client`
- 终端 B（中途取消）：`./build/cpp/05_action/action_client cancel`

预期输出：客户端先收到 8 条反馈（斐波那契 0,1,1,2,3,5,8,13），
最后结果是 21 并标记"已完成"。
加了 `cancel` 时，跑 1 秒后发出取消请求，服务端在下一轮循环里发现取消、
提前返回，结果是当时算到的值并标记"已取消"。

Python 版见 `python/05_action/`。
