#!/usr/bin/env python3
"""第 3 章 · 发布端（对应 ROS 2 官方教程里的 talker）

rclpy 版本长这样：
    node = rclpy.create_node("talker")
    pub  = node.create_publisher(Chatter, "topic", 10)
    pub.publish(msg)

这里除了消息类型换成 Chatter，形状是一样的。
DDS 实体的创建都在 node.py 的 Node.create_publisher 里，按注释看即可。

与 cpp/03_topic/talker.cpp 对应，可以对照着读。
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from node import Node   # noqa: E402

import TopicTypes   # noqa: E402  生成代码：TopicTypes.Chatter


def main():
    node = Node("talker")

    # 第二个参数是历史深度（对应 rclpy create_publisher 的 qos 参数）。
    pub = node.create_publisher("/demo/chatter")

    # 等订阅者出现再发。不等就发的话，样本可能在还没有订阅者时被丢掉。
    print("等待订阅者...")
    pub.wait_for_subscription()

    try:
        for i in range(10):
            msg = TopicTypes.Chatter()
            msg.index(i)
            msg.text(f"hello from python talker {i}")

            pub.publish(msg)
            print("发布 index=", i)
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy()

    print("发布端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
