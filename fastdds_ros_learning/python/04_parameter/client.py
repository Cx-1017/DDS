#!/usr/bin/env python3
"""第 4 章 · 参数客户端

对应 rclpy 里这一段：
    node.declare_parameter("speed", 1)
    node.get_parameter("speed").value
    node.set_parameters([Parameter("speed", 5)])
    node.get_parameter("speed").value        # 拿到新值

本程序演示参数的完整生命周期：声明 → 读取 → 修改 → 再读取。
想看「参数事件」那条广播通道，另开一个终端跑 monitor.py。

与 cpp/04_parameter/client.cpp 对应。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parameter import ParameterClient   # noqa: E402


def main():
    client = ParameterClient()

    print("等待参数服务端...")
    if not client.wait_for_server():
        print("没有等到服务端，请先在另一个终端启动 server.py", file=sys.stderr)
        return 1

    # 声明：如果参数已存在就保留原值，所以两次运行结果会不一样。
    ok, value = client.declare_parameter("speed", 1)
    if ok:
        print(f"declare speed = {value}")

    ok, value = client.get_parameter("speed")
    if ok:
        print(f"get     speed = {value}")

    ok, value = client.set_parameter("speed", 5)
    if ok:
        print("set     speed = 5")

    ok, value = client.get_parameter("speed")
    if ok:
        print(f"get     speed = {value}   <- 应该是 5")

    # 读一个没声明过的参数，服务端会如实返回失败。
    # 这种「失败要能被调用方看见」的设计很重要 —— 悄悄返回 0 会掩盖 bug。
    ok, _ = client.get_parameter("no_such_param")
    if not ok:
        print("get     no_such_param 失败（预期行为：参数未声明）")

    client.destroy()
    return 0


if __name__ == "__main__":
    sys.exit(main())
