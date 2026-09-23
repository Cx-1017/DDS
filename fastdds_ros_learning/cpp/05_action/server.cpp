// 第 5 章 · action 服务端（斐波那契）
//
// 对应 rclcpp_action 里这段：
//     auto server = rclcpp_action::create_server<Fibonacci>(node, "fibonacci",
//         handle_goal, handle_cancel, handle_accepted);
//
// 真正干活的代码就是下面那个 lambda —— 它对应 rclcpp_action 的
// execute_callback：一边算一边报反馈，每一轮检查有没有被取消。
//
// DDS 实体的创建都在同目录的 action.hpp 里，按 Endpoint 的注释看即可。

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "action.hpp"

static lesson05::ActionServer* g_server = nullptr;
static void on_sigint(int) { if (nullptr != g_server) g_server->stop(); }

int main()
{
    std::signal(SIGINT, on_sigint);

    lesson05::ActionServer server;
    g_server = &server;

    std::cout << "等待客户端...\n";
    server.wait_for_clients();

    server.spin([](lesson05::GoalHandle& handle) {
        FibonacciResult result;

        long a = 0, b = 1;
        for (long i = 0; i < handle.goal().order(); ++i)
        {
            // 每一轮都查一次取消 —— 这是 action 和 service 最本质的区别：
            // 长任务必须能被中途打断，所以服务端要主动"回头看"取消通道。
            // is_canceling() 是非阻塞的，不会把执行流程卡住。
            if (handle.is_canceling())
            {
                result.canceled(true);
                result.result(a);
                return result;
            }

            // 报进度。反馈可以发任意多次，客户端边收边显示。
            handle.publish_feedback(i, a);

            const long next = a + b;
            a = b;
            b = next;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        result.canceled(false);
        result.result(a);
        return result;
    });

    std::cout << "服务端退出\n";
    return 0;
}
