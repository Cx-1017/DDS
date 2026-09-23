// 第 5 章 · action 客户端
//
// 用法：
//     ./build/cpp/05_action/action_client           正常跑完 8 个数
//     ./build/cpp/05_action/action_client cancel    跑 1 秒后请求取消
//
// 对应 rclcpp_action 里这段：
//     auto client = rclcpp_action::create_client<Fibonacci>(node, "fibonacci");
//     auto goal = client->async_send_goal(request);
//     auto result = client->async_get_result(goal);     // 期间收 feedback
//     client->async_cancel_goal(goal);                  // 需要时取消

#include <chrono>
#include <iostream>
#include <string>

#include "action.hpp"

int main(int argc, char** argv)
{
    const bool request_cancel = (argc > 1) && (std::string(argv[1]) == "cancel");

    lesson05::ActionClient client;

    std::cout << "等待服务端...\n";
    if (!client.wait_for_server())
    {
        std::cerr << "没有等到服务端，请先在另一个终端启动 action_server\n";
        return 1;
    }

    // 发目标：让服务端生成 8 个斐波那契数。
    const unsigned long long goal_id = client.send_goal(8);
    std::cout << "已发送目标 #" << goal_id << "（order=8）\n";

    // wait_for_result 会一边收反馈、一边等结果。
    // 反馈回调在【主线程】里被调用（不是 DDS 线程），所以在这里调
    // cancel_goal() 是安全的，不需要额外加锁或开线程。
    const auto started = std::chrono::steady_clock::now();
    bool cancel_sent = false;

    FibonacciResult result;
    const bool ok = client.wait_for_result(
        goal_id, result,
        [&](const FibonacciFeedback& feedback) {
            std::cout << "反馈 " << feedback.sequence_index()
                      << ": " << feedback.partial_sequence() << "\n";

            // 跑到 1 秒的时候请求取消（只在命令行加了 cancel 时才这么做）。
            if (request_cancel && !cancel_sent &&
                std::chrono::steady_clock::now() - started > std::chrono::seconds(1))
            {
                std::cout << ">>> 请求取消目标 #" << goal_id << "\n";
                client.cancel_goal(goal_id);
                cancel_sent = true;
            }
        });

    if (!ok)
    {
        std::cerr << "没有等到结果\n";
        return 1;
    }

    std::cout << "结果: " << result.result()
              << (result.canceled() ? "（已取消）" : "（已完成）") << "\n";
    return 0;
}
