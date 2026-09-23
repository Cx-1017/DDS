#!/usr/bin/env python3
"""第 3 章 · 订阅端（对应 ROS 2 官方教程里的 listener）

rclpy 版本长这样：
    node = rclpy.create_node("listener")
    sub  = node.create_subscription(Chatter, "topic", callback, 10)
    rclpy.spin(node)

【关键差异】rclpy 的 spin() 是主线程去跑回调队列，回调确实是被 spin 驱动的。
这里的回调跑在 Fast DDS 的内部线程上，spin() 只负责把主线程阻塞住，
防止程序退出、节点被析构。所以就算不调 spin()，回调照样会被触发。

与 cpp/03_topic/listener.cpp 对应，可以对照着读。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from node import Node   # noqa: E402


def on_chatter(msg):
    """回调就是普通函数，形状和 rclpy 的 callback 一样。

    注意它运行在 DDS 内部线程上：
      * 不要在里面做长时间阻塞的操作，会拖住中间件的收发线程；
      * 里面碰到的共享数据要自己加锁（这个例子只是打印，不用加）。
    """
    print(f'回调收到 index={msg.index()} text="{msg.text()}"')


def main():
    node = Node("listener")
    node.create_subscription("/demo/chatter", on_chatter)

    print("订阅端已就绪，等待消息（Ctrl-C 退出）...")
    try:
        node.spin()
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy()

    print("\n订阅端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
