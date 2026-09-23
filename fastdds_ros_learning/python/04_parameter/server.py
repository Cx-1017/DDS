#!/usr/bin/env python3
"""第 4 章 · 参数服务端

对应 rclpy 里那个「持有参数」的节点。它做三件事：
    收 ParamRequest → 查/改自己的参数表 → 回 ParamResponse
    改参数时额外广播一条 ParamEvent

DDS 实体的创建都在同目录的 parameter.py 里，按 Endpoint 的注释看即可。
与 cpp/04_parameter/server.cpp 对应。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parameter import ParameterServer   # noqa: E402


def main():
    server = ParameterServer()

    print("参数服务端就绪，等待客户端...")
    server.wait_for_clients()

    try:
        server.spin()
    except KeyboardInterrupt:
        pass
    finally:
        server.destroy()

    print("参数服务端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
