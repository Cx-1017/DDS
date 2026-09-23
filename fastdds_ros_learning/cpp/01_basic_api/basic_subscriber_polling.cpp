// 第 1 章 · Fast DDS 裸 API —— 订阅端之二：主线程轮询
//
// 和 basic_subscriber_listener.cpp 只差两点：
//   1. create_datareader() 的监听器参数传 nullptr
//   2. 取数据变成主线程里的显式循环：等 → 取 → 处理
//
// 对初学者来说这个顺序最好懂，因为控制流全在 main 里，不需要理解
// "回调在哪个线程上跑"。代价是要自己管循环和超时。
//
// 三个和取数相关的 API，用哪个取决于你想不想保留缓存里的数据：
//   wait_for_unread_message(超时)  阻塞等待，直到有未读数据（这里用它）
//                                  返回 bool：有数据 true / 超时 false
//   take_next_sample(&s, &info)    取一条，并从缓存里删掉
//   read_next_sample(&s, &info)    取一条，仍留在缓存里
//
// reader 还有 get_unread_count()，但用它做忙轮询会白白烧 CPU，
// 阻塞式的 wait_for_unread_message() 才是正解。

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

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

// 轮询模式不需要 DataReaderListener 来取数据，但监听器还有别的用处：
// 在这里只用来感知"发布端来了/走了"。也可以完全不要监听器。
class MatchListener : public DataReaderListener
{
public:
    void on_subscription_matched(DataReader*, const SubscriptionMatchedStatus& info) override
    {
        std::cout << (info.current_count_change > 0 ? "发现发布端" : "发布端离开")
                  << "，当前匹配数=" << info.current_count << "\n";
    }
};

int main()
{
    std::signal(SIGINT, on_sigint);

    // ── 1~4 步：工厂 → 参与者 → 注册类型 → Topic（与另两个程序相同）───────
    DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();
    DomainParticipant* participant = factory->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (nullptr == participant) { std::cerr << "创建 DomainParticipant 失败\n"; return 1; }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);   // 必须在 create_topic 之前

    Topic* topic = participant->create_topic(
        "HelloWorldTopic", type.get_type_name(), TOPIC_QOS_DEFAULT);
    if (nullptr == topic) { std::cerr << "创建 Topic 失败\n"; return 1; }

    // ── 5. Subscriber 和 DataReader ───────────────────────────────────────
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (nullptr == subscriber) { std::cerr << "创建 Subscriber 失败\n"; return 1; }

    static MatchListener listener;
    DataReader* reader = subscriber->create_datareader(topic, DATAREADER_QOS_DEFAULT, &listener);
    if (nullptr == reader) { std::cerr << "创建 DataReader 失败\n"; return 1; }

    // ── 6. 主线程轮询 ─────────────────────────────────────────────────────
    std::cout << "订阅端已就绪，轮询等待数据（Ctrl-C 退出）...\n";
    HelloWorld sample;
    SampleInfo info;

    while (g_running)
    {
        // 阻塞等待，直到 reader 上有未读数据。
        // 【注意返回类型】这个函数返回的是 bool 而不是 ReturnCode_t：
        // 有数据返回 true，超时返回 false。别习惯性地拿它和 RETCODE_OK 比 ——
        // RETCODE_OK 是 0 而 true 是 1，那样写会导致"有数据时反而跳过"。
        Duration_t timeout(1, 0);   // 1 秒 0 纳秒
        if (!reader->wait_for_unread_message(timeout))
        {
            continue;   // 超时，回去继续等
        }

        // 等到之后仍然要用 while 排空，因为一次可能来了多条。
        while (RETCODE_OK == reader->take_next_sample(&sample, &info))
        {
            if (!info.valid_data)   // dispose / unregister 等生命周期事件，不是数据
            {
                continue;
            }
            std::cout << "收到 index=" << sample.index()
                      << " message=\"" << sample.message() << "\"\n";
        }
    }

    // ── 7. 清理 ───────────────────────────────────────────────────────────
    participant->delete_contained_entities();
    factory->delete_participant(participant);
    std::cout << "\n订阅端退出\n";
    return 0;
}
