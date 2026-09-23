#!/usr/bin/env python3
"""第 5 章 · action 服务端（斐波那契）

对应 rclpy 里这段：
    ActionServer(node, Fibonacci, "fibonacci", execute_callback)

真正干活的代码就是下面那个 handle_goal 函数 —— 它对应 execute_callback：
一边算一边报反馈，每一轮检查有没有被取消。

DDS 实体的创建都在同目录的 action.py 里，按 Endpoint 的注释看即可。
与 cpp/05_action/server.cpp 对应。
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from action import ActionServer   # noqa: E402

import ActionTypes   # noqa: E402  生成代码：ActionTypes.FibonacciResult


def handle_goal(handle):
    """对应 rclpy 的 execute_callback。"""

    result = ActionTypes.FibonacciResult()

    a, b = 0, 1
    for i in range(handle.goal.order()):
        # 每一轮都查一次取消 —— 这是 action 和 service 最本质的区别：
        # 长任务必须能被中途打断，所以服务端要主动「回头看」取消通道。
        # is_canceling() 是非阻塞的，不会把执行流程卡住。
        if handle.is_canceling():
            result.canceled(True)
            result.result(a)
            return result

        # 报进度。反馈可以发任意多次，客户端边收边显示。
        handle.publish_feedback(i, a)

        a, b = b, a + b
        time.sleep(0.5)

    result.canceled(False)
    result.result(a)
    return result


def main():
    server = ActionServer()

    print("等待客户端...")
    server.wait_for_clients()

    try:
        server.spin(handle_goal)
    except KeyboardInterrupt:
        pass
    finally:
        server.destroy()

    print("服务端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
