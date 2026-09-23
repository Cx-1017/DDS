#!/usr/bin/env python3
"""第 4 章 · 参数事件监听

对应 ROS 2 里的 /parameter_events 话题。

这条 Topic 和前两条（request/response）性质不同：
  * request/response 是点对点的 —— 谁发的请求，响应就回给谁；
  * parameter_events 是广播 —— 任何订阅了它的进程都能感知到参数变化，
    不需要它请求过什么。

所以在终端 C 单独跑这个程序，然后在终端 B 跑 client.py 改参数，
就能看到它收到通知。这就是 DDS 发布的「一对多」特性在参数系统里的用法。

用的是和第 1 章轮询订阅端一样的写法：wait_for_unread_message 阻塞等待。
与 cpp/04_parameter/monitor.cpp 对应。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parameter import Endpoint, create_participant, delete_participant   # noqa: E402

import datetime   # noqa: E402

import ParameterTypes   # noqa: E402  生成代码：ParameterTypes.ParamEvent / ParamEventPubSubType


def main():
    # 只收不发，所以只需要一个 reading 端点。
    # 参与者要自己建 —— 这里没有封装成类，正好复习一下裸 API。
    participant = create_participant()
    events = Endpoint(participant, "/demo/parameter_events",
                      ParameterTypes.ParamEventPubSubType,
                      ParameterTypes.ParamEvent, reading=True)

    print("监听 /demo/parameter_events（Ctrl-C 退出）...")
    try:
        while True:
            event = events.take(datetime.timedelta(milliseconds=200))
            if event is not None:
                print(f"[事件] 参数 {event.name()} 变成了 {event.value()}")
    except KeyboardInterrupt:
        pass
    finally:
        delete_participant(participant)

    print("\n监听退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
