// 第 3 章 · 发布端（对应 ROS 2 官方教程里的 talker）
//
// ROS 2 版本长这样：
//     auto node = std::make_shared<rclcpp::Node>("talker");
//     auto pub  = node->create_publisher<Chatter>("topic", 10);
//     pub->publish(msg);
//
// 这里除了把消息类型换成 Chatter，形状是一样的。
// DDS 实体的创建都在 node.hpp 的 Node::create_publisher 里，按注释看即可。

#include <chrono>
#include <iostream>
#include <thread>

#include "node.hpp"

int main()
{
    lesson03::Node node("talker");

    // 第二个参数是历史深度（对应 ROS 2 create_publisher 的 qos 参数）。
    auto pub = node.create_publisher("/demo/chatter");

    // 等订阅者出现再发。不等就发的话，样本可能在还没有订阅者时被丢掉。
    std::cout << "等待订阅者...\n";
    pub->wait_for_subscription();

    for (unsigned long i = 0; i < 10; ++i)
    {
        Chatter msg;
        msg.index(i);
        msg.text("hello from cpp talker " + std::to_string(i));

        pub->publish(msg);
        std::cout << "发布 index=" << i << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "发布端退出\n";
    return 0;
}
