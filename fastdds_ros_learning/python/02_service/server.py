#!/usr/bin/env python3
"""第 2 章 · 服务端

对应 rclpy 里这样一段代码：
    service = node.create_service(Add, "add", handler)
    rclpy.spin(node)

DDS 的细节都收在同目录的 service.py 里，本文件只关心「这个服务做什么」。
想学 DDS 实体怎么建，看 service.py 里 Endpoint 的构造函数。

与 cpp/02_service/server.cpp 对应，可以对照着读。
"""

import sys
from pathlib import Path

# service.py 会把生成类型模块所在的 build 目录加进 sys.path，
# 所以这一句要放在 import ServiceTypes 之前。
sys.path.insert(0, str(Path(__file__).resolve().parent))
from service import Service   # noqa: E402

import ServiceTypes   # noqa: E402  生成代码：ServiceTypes.AddResponse


def handle_add(request):
    """服务提供的能力：把两个数加起来。

    request_id 不用自己填，Service.spin() 会把请求里的 id 原样带进响应。
    """
    response = ServiceTypes.AddResponse()
    response.sum(request.a() + request.b())
    return response


def main():
    # 两条 Topic 的名字。rclpy 里一个服务名 "add"，在 DDS 上展开成两条 Topic：
    # 一条收请求，一条发响应。
    service = Service("/demo/add/request", "/demo/add/response")

    # 等客户端出现再开始收请求。这是 DDS 上做 RPC 最容易踩的坑：
    # 两端还没互相发现时发出去的样本会被直接丢掉，客户端只能等到超时。
    print("等待客户端...")
    service.wait_for_clients()

    try:
        service.spin(handle_add)
    except KeyboardInterrupt:
        pass
    finally:
        service.destroy()

    print("服务端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
