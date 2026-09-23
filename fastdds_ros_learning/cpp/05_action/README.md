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
