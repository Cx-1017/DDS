// 第 4 章 · 参数事件监听
//
// 对应 ROS 2 里的 /parameter_events 话题。
//
// 这条 Topic 和前两条（request/response）性质不同：
//   * request/response 是点对点的 —— 谁发的请求，响应就回给谁；
//   * parameter_events 是广播 —— 任何订阅了它的进程都能感知到参数变化，
//     不需要它请求过什么。
//
// 所以在终端 C 单独跑这个程序，然后在终端 B 跑 param_client 改参数，
// 就能看到它收到通知。这就是 DDS 发布的"一对多"特性在参数系统里的用法。
//
// 用的是和第 1 章轮询订阅端一样的写法：wait_for_unread_message 阻塞等待。

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>

#include "parameter.hpp"

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

int main()
{
    std::signal(SIGINT, on_sigint);

    // 只收不发，所以只需要一个 reading 端点。
    // 参与者要自己建 —— 这里没有封装成类，正好复习一下裸 API。
    auto* participant = lesson04::create_participant();
    lesson04::Endpoint<ParamEvent, ParamEventPubSubType> events(
        participant, "/demo/parameter_events", true);

    std::cout << "监听 /demo/parameter_events（Ctrl-C 退出）...\n";

    ParamEvent event;
    while (g_running)
    {
        if (events.take(event, std::chrono::milliseconds(200)))
        {
            std::cout << "[事件] 参数 " << event.name()
                      << " 变成了 " << event.value() << "\n";
        }
    }

    lesson04::delete_participant(participant);
    std::cout << "\n监听退出\n";
    return 0;
}
