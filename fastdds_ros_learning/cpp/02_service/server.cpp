// 第 2 章 · 服务端
//
// 对应 ROS 2 里这样一段代码：
//     auto service = node->create_service<Add>("add", handler);
//     rclcpp::spin(node);
//
// DDS 的细节都收在同目录的 service.hpp 里，本文件只关心"这个服务做什么"。
// 想学 DDS 实体怎么建，看 service.hpp 里 Endpoint 的构造函数。

#include <csignal>
#include <iostream>

#include "service.hpp"

// Ctrl-C 时让 spin() 的循环退出。
static lesson02::Service* g_service = nullptr;
static void on_sigint(int) { if (nullptr != g_service) g_service->stop(); }

int main()
{
    std::signal(SIGINT, on_sigint);

    // 两条 Topic 的名字。ROS 2 里一个服务名 "add"，在 DDS 上展开成两条 Topic：
    // 一条收请求，一条发响应。
    lesson02::Service service("/demo/add/request", "/demo/add/response");
    g_service = &service;

    // 等客户端出现再开始收请求。这是 DDS 上做 RPC 最容易踩的坑：
    // 两端还没互相发现时发出去的样本会被直接丢掉，客户端只能等到超时。
    std::cout << "等待客户端...\n";
    service.wait_for_clients();

    // handler 就是"这个服务提供的能力"，写业务逻辑即可。
    // request_id 不用自己填，spin() 会把请求里的 id 原样带进响应。
    service.spin([](const AddRequest& request) {
        AddResponse response;
        response.sum(request.a() + request.b());
        return response;
    });

    std::cout << "服务端退出\n";
    return 0;
}
