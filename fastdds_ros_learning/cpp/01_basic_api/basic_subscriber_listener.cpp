// 第 1 章 · Fast DDS 裸 API —— 订阅端之一：listener 回调
//
// 与 basic_publisher.cpp 的创建流程完全对称，只是把
//   Publisher  → Subscriber
//   DataWriter → DataReader
// 并且 create_datareader() 多传一个 DataReaderListener*。
//
// ── 线程模型：数据是谁在什么时候取走的 ─────────────────────────────────────
//
//   Fast DDS 内部线程                          主线程
//   ──────────────────────                     ──────────────────
//         │                                          │
//   收到一个样本                                     │
//         │                                          │
//         ▼                                          │
//   on_data_available(reader)  ← 自动回调            │
//         │      ▲                                   │
//         │      └─ 这个函数【不是你调的】，是中间件调的
//         │                                          │
//         ├─ while (take_next_sample(...) == OK)     │
//         │      排空缓存，处理每一条                 │
//         │                                          │
//         ▼                                          ▼
//    （回调返回，线程继续干别的）         while (g_running) sleep(200ms)
//                                          ↑ 主线程只是等在这里，
//                                            防止 main 返回把对象析构掉
//
// 三个必须记住的结论：
//   1. 取数据的代码不在 main 里，而在中间件线程上被并发调用 ——
//      回调里碰到的共享数据要自己加锁。
//   2. 回调里不能长时间阻塞，否则会拖住 DDS 的收发线程。
//   3. listener 对象的生命周期必须覆盖 DataReader —— reader 只持有裸指针，
//      listener 先析构就会让中间件回调到已释放的内存。
//      所以下面把它声明成 static，活到程序结束。
//
// ── 和轮询方式的唯一区别 ───────────────────────────────────────────────────
//   listener：create_datareader(topic, qos, &listener)   ← 传 &listener
//   轮询    ：create_datareader(topic, qos, nullptr)     ← 传 nullptr，
//             然后自己在主线程里循环 wait_for_unread_message() + take
//
// 对照程序：basic_subscriber_polling.cpp。两种方式没有优劣，看场景选。

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "BasicTypes.hpp"
#include "BasicTypesPubSubTypes.hpp"

using namespace eprosima::fastdds::dds;

// Ctrl-C 时把标志位置起来，让 main 里的等待循环退出。
static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

// ── 监听器 ─────────────────────────────────────────────────────────────────
// 继承 DataReaderListener，按需覆盖关心的事件。这里覆盖两个：
//   on_data_available       reader 上有新数据可取
//   on_subscription_matched 匹配状态变化（发现/失去发布端）
//
// listener 的成员函数由 DDS 内部线程调用，所以返回值必须是 void，
// 也不能在里面长时间阻塞，否则会卡住中间件的收发线程。
class HelloWorldListener : public DataReaderListener
{
public:
    void on_data_available(DataReader* reader) override
    {
        HelloWorld sample;
        SampleInfo info;

        // take_next_sample() 取出一条并把它从 reader 的缓存里【删除】。
        // 想取出来但保留在缓存里，用 read_next_sample()。
        //
        // 必须用 while 循环排空：回调只保证"此刻 reader 上有数据"，
        // 并不保证只有一条。一次只取一条的话，积压的数据要等下一次
        // 回调才处理得完，高频率下发会越来越落后。
        while (RETCODE_OK == reader->take_next_sample(&sample, &info))
        {
            // info.valid_data 为 false 表示这条不是真实数据，而是
            // dispose（标记实例已删除）或 unregister 之类的生命周期事件。
            // 不判断的话会把它们当成正常消息打印出来。
            if (!info.valid_data)
            {
                continue;
            }
            std::cout << "收到 index=" << sample.index()
                      << " message=\"" << sample.message() << "\"\n";
        }
    }

    void on_subscription_matched(DataReader*, const SubscriptionMatchedStatus& info) override
    {
        // current_count_change > 0 表示新增了一个发布端，< 0 表示有发布端离开。
        std::cout << (info.current_count_change > 0 ? "发现发布端" : "发布端离开")
                  << "，当前匹配数=" << info.current_count << "\n";
    }
};

int main()
{
    std::signal(SIGINT, on_sigint);

    // ── 1~4 步和发布端一样：工厂 → 参与者 → 注册类型 → Topic ──────────────
    DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();
    DomainParticipant* participant = factory->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (nullptr == participant) { std::cerr << "创建 DomainParticipant 失败\n"; return 1; }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);   // 必须在 create_topic 之前

    // 话题名和类型名必须和发布端完全一致，否则匹配不上。
    Topic* topic = participant->create_topic(
        "HelloWorldTopic", type.get_type_name(), TOPIC_QOS_DEFAULT);
    if (nullptr == topic) { std::cerr << "创建 Topic 失败\n"; return 1; }

    // ── 5. Subscriber 和 DataReader ───────────────────────────────────────
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (nullptr == subscriber) { std::cerr << "创建 Subscriber 失败\n"; return 1; }

    // 这个 listener 对象必须活得比 reader 久 —— reader 会持有它的指针，
    // 并在自己的线程里调用它。所以它声明在 main 的栈上、在 reader 之前
    // 析构是不行的；这里它一直活到 main 结束，是安全的。
    static HelloWorldListener listener;

    // create_datareader(话题, QoS, 监听器)。
    // 第三个参数传监听器就是"数据到了回调我"；传 nullptr 就变成轮询模式，
    // 需要自己调 wait_for_unread_message() / take_next_sample()。
    DataReader* reader = subscriber->create_datareader(topic, DATAREADER_QOS_DEFAULT, &listener);
    if (nullptr == reader) { std::cerr << "创建 DataReader 失败\n"; return 1; }

    // ── 6. 等待 ───────────────────────────────────────────────────────────
    // 数据在 DDS 内部线程里被回调处理，主线程只需要等在这里。
    std::cout << "订阅端已就绪，等待数据（Ctrl-C 退出）...\n";
    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ── 7. 清理 ───────────────────────────────────────────────────────────
    participant->delete_contained_entities();
    factory->delete_participant(participant);
    std::cout << "\n订阅端退出\n";
    return 0;
}
