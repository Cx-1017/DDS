// 第 3 章 · 订阅端（对应 ROS 2 官方教程里的 listener）
//
// ROS 2 版本长这样：
//     auto node = std::make_shared<rclcpp::Node>("listener");
//     auto sub  = node->create_subscription<Chatter>("topic", 10, callback);
//     rclcpp::spin(node);
//
// 【关键差异】ROS 2 的 spin() 是主线程去跑回调队列，回调确实是被 spin 驱动的。
// 这里的回调跑在 Fast DDS 的内部线程上，spin() 只负责把主线程阻塞住，
// 防止 main 返回、节点被析构。所以就算不调 spin()，回调照样会被触发。

#include <csignal>
#include <iostream>

#include "node.hpp"

static lesson03::Node* g_node = nullptr;
static void on_sigint(int) { if (nullptr != g_node) g_node->shutdown(); }

int main()
{
    std::signal(SIGINT, on_sigint);

    lesson03::Node node("listener");
    g_node = &node;

    // 回调就是普通函数或 lambda，形状和 rclcpp 的 callback 一样。
    // 注意它运行在 DDS 内部线程上：
    //   * 不要在里面做长时间阻塞的操作，会拖住中间件的收发线程；
    //   * 里面碰到的共享数据要自己加锁（这个例子只是打印，不用加）。
    auto sub = node.create_subscription("/demo/chatter", [](const Chatter& msg) {
        std::cout << "回调收到 index=" << msg.index()
                  << " text=\"" << msg.text() << "\"\n";
    });

    std::cout << "订阅端已就绪，等待消息（Ctrl-C 退出）...\n";
    node.spin();

    std::cout << "\n订阅端退出\n";
    return 0;
}
