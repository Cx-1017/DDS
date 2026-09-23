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
