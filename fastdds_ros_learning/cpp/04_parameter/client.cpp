// 第 4 章 · 参数客户端
//
// 对应 ROS 2 里这一段：
//     node->declare_parameter("speed", 1);
//     node->get_parameter("speed");
//     node->set_parameter({"speed", 5});
//     node->get_parameter("speed");     // 拿到新值
//
// 本程序演示参数的完整生命周期：声明 → 读取 → 修改 → 再读取。
// 想看"参数事件"那条广播通道，另开一个终端跑 param_monitor。

#include <iostream>

#include "parameter.hpp"

int main()
{
    lesson04::ParameterClient client;

    std::cout << "等待参数服务端...\n";
    if (!client.wait_for_server())
    {
        std::cerr << "没有等到服务端，请先在另一个终端启动 param_server\n";
        return 1;
    }

    long value = 0;

    // 声明：如果参数已存在就保留原值，所以两次运行结果会不一样。
    if (client.declare_parameter("speed", 1, value))
    {
        std::cout << "declare speed = " << value << "\n";
    }

    if (client.get_parameter("speed", value))
    {
        std::cout << "get     speed = " << value << "\n";
    }

    if (client.set_parameter("speed", 5))
    {
        std::cout << "set     speed = 5\n";
    }

    if (client.get_parameter("speed", value))
    {
        std::cout << "get     speed = " << value << "   <- 应该是 5\n";
    }

    // 读一个没声明过的参数，服务端会如实返回失败。
    // 这种"失败要能被调用方看见"的设计很重要 —— 悄悄返回 0 会掩盖 bug。
    if (!client.get_parameter("no_such_param", value))
    {
        std::cout << "get     no_such_param 失败（预期行为：参数未声明）\n";
    }

    return 0;
}
