// 第 4 章 · 参数服务端
//
// 对应 rclpy/rclcpp 里那个"持有参数"的节点。它做三件事：
//   收 ParamRequest → 查/改自己的参数表 → 回 ParamResponse
//   改参数时额外广播一条 ParamEvent
//
// DDS 实体的创建都在同目录的 parameter.hpp 里，按 Endpoint 的注释看即可。

#include <csignal>
#include <iostream>

#include "parameter.hpp"

static lesson04::ParameterServer* g_server = nullptr;
static void on_sigint(int) { if (nullptr != g_server) g_server->stop(); }

int main()
{
    std::signal(SIGINT, on_sigint);

    lesson04::ParameterServer server;
    g_server = &server;

    std::cout << "参数服务端就绪，等待客户端...\n";
    server.wait_for_clients();
    server.spin();

    std::cout << "参数服务端退出\n";
    return 0;
}
