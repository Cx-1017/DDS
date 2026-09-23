#!/usr/bin/env python3
"""第 2 章 · 客户端

对应 rclpy 里这样一段代码：
    client = node.create_client(Add, "add")
    client.wait_for_service()
    future = client.call_async(request)
    response = future.result()

这里的 call() 是同步的（rclpy 的 call_async() 返回 future），为的是把
「请求怎么和响应配对」这件事讲清楚 —— 见 service.py 里 Client.call 的注释。

与 cpp/02_service/client.cpp 对应，可以对照着读。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from service import Client   # noqa: E402


def main():
    client = Client("/demo/add/request", "/demo/add/response")

    # 先等服务端。不等就发的话，请求会在两端还没发现彼此的时候丢掉。
    print("等待服务端...")
    if not client.wait_for_service():
        print("没有等到服务端，请先在另一个终端启动 server", file=sys.stderr)
        return 1

    response = client.call(20, 22)
    if response is None:
        print("调用超时，没有收到匹配的响应", file=sys.stderr)
        return 1

    print(f"add(20, 22) = {response.sum()}")
    return 0 if response.sum() == 42 else 1


if __name__ == "__main__":
    sys.exit(main())
