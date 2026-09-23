// 第 2 章 · 客户端
//
// 对应 ROS 2 里这样一段代码：
//     auto client = node->create_client<Add>("add");
//     client->wait_for_service();
//     auto future = client->async_send_request(request);
//     auto response = future.get();
//
// 这里的 call() 是同步的（ROS 2 是异步返回 future），为的是把
// "请求怎么和响应配对"这件事讲清楚 —— 见 service.hpp 里 Client::call 的注释。

#include <iostream>

#include "service.hpp"

int main()
{
    lesson02::Client client("/demo/add/request", "/demo/add/response");

    // 先等服务端。不等就发的话，请求会在两端还没发现彼此的时候丢掉。
    std::cout << "等待服务端...\n";
    if (!client.wait_for_service())
    {
        std::cerr << "没有等到服务端，请先在另一个终端启动 server\n";
        return 1;
    }

    AddResponse response;
    if (client.call(20, 22, response))
    {
        std::cout << "add(20, 22) = " << response.sum() << "\n";
        return response.sum() == 42 ? 0 : 1;
    }

    std::cerr << "调用超时，没有收到匹配的响应\n";
    return 1;
}
