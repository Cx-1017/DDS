#!/usr/bin/env python3
"""第 5 章 · action 客户端

用法：
    python3 python/05_action/client.py           正常跑完 8 个数
    python3 python/05_action/client.py cancel    跑 1 秒后请求取消

对应 rclpy 里这段：
    client = ActionClient(node, Fibonacci, "fibonacci")
    goal_handle = client.send_goal_async(goal)        # 发目标
    client.cancel_goal_async(goal_handle)             # 需要时取消
    result = client.get_result_async(goal_handle)     # 期间收 feedback

与 cpp/05_action/client.cpp 对应。
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from action import ActionClient   # noqa: E402


def main():
    request_cancel = len(sys.argv) > 1 and sys.argv[1] == "cancel"

    client = ActionClient()

    print("等待服务端...")
    if not client.wait_for_server():
        print("没有等到服务端，请先在另一个终端启动 server.py", file=sys.stderr)
        return 1

    # 发目标：让服务端生成 8 个斐波那契数。
    goal_id = client.send_goal(8)
    print(f"已发送目标 #{goal_id}（order=8）")

    # wait_for_result 会一边收反馈、一边等结果。
    # 反馈回调在【主线程】里被调用，所以在这里调 cancel_goal() 是安全的，
    # 不需要额外开线程。
    started = time.monotonic()
    state = {"cancel_sent": False}

    def on_feedback(feedback):
        print(f"反馈 {feedback.sequence_index()}: {feedback.partial_sequence()}")

        # 跑到 1 秒的时候请求取消（只在命令行加了 cancel 时才这么做）。
        if request_cancel and not state["cancel_sent"] and time.monotonic() - started > 1:
            print(f">>> 请求取消目标 #{goal_id}")
            client.cancel_goal(goal_id)
            state["cancel_sent"] = True

    result = client.wait_for_result(goal_id, on_feedback)
    if result is None:
        print("没有等到结果", file=sys.stderr)
        client.destroy()
        return 1

    state_text = "已取消" if result.canceled() else "已完成"
    print(f"结果: {result.result()}（{state_text}）")

    client.destroy()
    return 0


if __name__ == "__main__":
    sys.exit(main())
