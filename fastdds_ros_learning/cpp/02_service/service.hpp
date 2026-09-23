#pragma once

// 第 2 章 · 用 DDS 实现 ROS 2 风格的服务
//
// ── ROS 2 里的服务长什么样 ─────────────────────────────────────────────────
//   rclcpp:  node->create_service<Add>("add", handler);
//            node->create_client<Add>("add");
//            client->async_send_request(request);
//   服务是"请求-响应"语义：客户端发一个请求，服务端算完把结果发回来。
//
// ── DDS 里怎么拼出来 ───────────────────────────────────────────────────────
//   DDS 只有发布订阅，没有 RPC。服务要用两条 Topic 拼：
//
//     客户端 --AddRequest-->  /demo/add/request  --> 服务端
//     客户端 <--AddResponse-- /demo/add/response <-- 服务端
//
//   两条 Topic 之间靠 request_id 关联：响应把请求的 id 原样带回来，
//   客户端只认 id 对得上的那条响应，这样多个请求并发也不会串味。
//
// ── 一次 call() 的时序 ─────────────────────────────────────────────────────
//
//   Client                                    Server
//     │                                          │
//     │ ① wait_for_service()                     │  ← 服务端要先起。
//     │     轮询两条 Topic 的匹配数                │    不等就发，样本会
//     │ <───────── 发现完成 ─────────────────────>│    在没匹配上时被丢掉
//     │                                          │
//     │ ② request.request_id(1)                  │
//     │    request.a(20) / request.b(22)         │
//     │                                          │
//     │ ③ requests_.publish(request)             │
//     │ ──────── /demo/add/request ────────────> │ ④ requests_.take()
//     │                                          │
//     │                                          │ ⑤ handler(request)
//     │                                          │      sum = 42
//     │                                          │
//     │                                          │ ⑥ response.request_id(
//     │                                          │       request.request_id())
//     │                                          │    ⚠ 配对的全部就在这里
//     │ ⑧ responses_.take()                      │
//     │    检查 request_id == 1 ？                │ ⑦ responses_.publish(...)
//     │ ──────── /demo/add/response ───────────< │
//     │    对得上 → 返回；对不上 → 丢掉继续等       │
//     │                                          │
//
//   第 ⑥ 步是整章的核心：服务端唯一的义务就是"把 id 原样带回"。
//
// ── 本文件的结构 ───────────────────────────────────────────────────────────
//   1. Endpoint<>  —— 一个最小的话题端点（约 90 行）。
//                     【这是全章唯一直接碰 DDS 的地方】，构造函数里逐行
//                     注释了每一步对应哪个 DDS 实体。想学 DDS 就看这里。
//   2. Service     —— 服务端，对应 rclcpp 的 create_service + spin
//   3. Client      —— 客户端，对应 rclcpp 的 create_client + async_send_request
//
//   Service / Client 只是在 Endpoint 之上加了一层 ROS 风格的 API，
//   本身不含新的 DDS 知识。
//
// ── 三层结构，每层加的东西不多 ─────────────────────────────────────────────
//
//   ┌─ Service / Client ────────────────────────────────────┐
//   │  只做协议逻辑：request_id 怎么发、怎么配对、何时放弃       │
//   │  （这两个类里【没有一行】DDS 调用）                       │
//   └──────────────────────┬────────────────────────────────┘
//                          │ 用两个 Endpoint，
//                          │ 一个方向收、一个方向发
//   ┌──────────────────────▼────────────────────────────────┐
//   │  Endpoint<Sample, PubSubType>                         │
//   │  register_type → create_topic → create_publisher/     │
//   │  create_subscriber → create_datawriter/datareader     │
//   │  ★ 全章唯一直接碰 DDS 的地方 ★                          │
//   └──────────────────────┬────────────────────────────────┘
//                          │
//   ┌──────────────────────▼────────────────────────────────┐
//   │  Fast DDS 中间件（domain participant / RTPS / 发现协议） │
//   └───────────────────────────────────────────────────────┘
//
//   每个 Endpoint 在参与者下面建的完整链路：
//
//     DomainParticipant
//       ├── TypeSupport(AddRequest)     register_type 必须在建 Topic 之前
//       ├── Topic /demo/add/request
//       └── Subscriber └── DataReader   ← 收
//     （另一个 Endpoint 同理，但方向是 Publisher └── DataWriter → 发）

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "ServiceTypes.hpp"
#include "ServiceTypesPubSubTypes.hpp"

namespace lesson02 {

using namespace eprosima::fastdds::dds;

// ── 时间转换的小工具 ───────────────────────────────────────────────────────
// Fast DDS 的时间类型 Duration_t 内部是（int32 秒, uint32 纳秒）两个字段。
// 所以【不能】简单地把毫秒乘 1000000 塞进第二个参数：5000ms 换算出来是
// 5e9 纳秒，超过了 uint32 的上限（约 4.29e9），会静默溢出成 0.7 秒。
// Duration_t 有一个接受 long double 秒的构造函数，小数部分自动转成纳秒，
// 用它最省事，也不会有溢出问题。
inline Duration_t to_duration(std::chrono::milliseconds timeout)
{
    return Duration_t(static_cast<long double>(timeout.count()) / 1000.0L);
}

// ── 参与者的创建与销毁 ─────────────────────────────────────────────────────
// 每个"节点"都从这一步开始：拿到工厂，建一个参与者。
// 域号相同的进程才能互相发现，本章所有程序都用域 0。
inline DomainParticipant* create_participant(int domain_id = 0)
{
    // DomainParticipantFactory 是进程级单例。
    DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();
    // create_participant(域号, QoS)。PARTICIPANT_QOS_DEFAULT 表示"用默认值"，
    // 不需要先取一遍默认 QoS。
    return factory->create_participant(domain_id, PARTICIPANT_QOS_DEFAULT);
}

inline void delete_participant(DomainParticipant* participant)
{
    if (nullptr == participant)
    {
        return;
    }
    // delete_contained_entities() 会连带删掉这个参与者下面的
    // Topic / Publisher / Subscriber / DataWriter / DataReader，
    // 所以不用逐个 delete 那些指针。
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
}

// ── 端点：一个话题上的"发"或"收" ───────────────────────────────────────────
// 模板参数是 fastddsgen 生成的两个类：
//   SampleT      —— 数据结构，例如 AddRequest
//   PubSubTypeT  —— DDS 类型适配类，例如 AddRequestPubSubType
//
// reading=true 建 DataReader（收），false 建 DataWriter（发）。
template <class SampleT, class PubSubTypeT>
class Endpoint
{
public:
    Endpoint(DomainParticipant* participant, const std::string& topic_name, bool reading)
        : participant_(participant), reading_(reading)
    {
        // ① 注册类型：把生成的数据结构和 DDS 中间件关联起来。
        //    register_type() 内部还会顺带注册类型的自描述信息（TypeObject），
        //    供发现阶段使用。
        TypeSupport type(new PubSubTypeT());
        participant_->register_type(type);

        // ② 建 Topic：把一个【名字】和一个【类型】绑在一起。
        //    这一步【必须】在 register_type 之后 —— create_topic 会去查找
        //    已注册的类型，找不到就返回 nullptr。
        //    三个参数：话题名、类型名、QoS。
        topic_ = participant_->create_topic(
            topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);

        if (reading_)
        {
            // ③ 订阅侧：Subscriber 是容器，DataReader 才是真正收数据的实体。
            //    监听器传 nullptr，也就是不用回调，改成主动轮询
            //    —— 取数逻辑见下面的 take()。
            subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
            reader_ = subscriber_->create_datareader(topic_, DATAREADER_QOS_DEFAULT, nullptr);
        }
        else
        {
            // ③ 发布侧：Publisher 是容器，DataWriter 才是真正发数据的实体。
            publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
            writer_ = publisher_->create_datawriter(topic_, DATAWRITER_QOS_DEFAULT);
        }
    }

    // 注意：这里【不删】topic_ —— 它由参与者的 delete_contained_entities()
    // 统一回收，提前删掉会让共用同一个参与者的其它端点出错。
    ~Endpoint() = default;

    // 发一条。write() 收的是 const void*，所以直接传 const 引用取地址即可。
    // 返回 RETCODE_OK 只代表中间件收下了，不代表对端收到了 —— DDS 是异步的。
    void publish(const SampleT& sample)
    {
        writer_->write(&sample);
    }

    // 阻塞等待最多 timeout，取出一条数据。
    // 返回 false 表示超时，或者只等到了生命周期事件（没有真实数据）。
    bool take(SampleT& out, std::chrono::milliseconds timeout)
    {
        const Duration_t wait_for = to_duration(timeout);

        // 【注意返回类型】wait_for_unread_message() 返回的是 bool 而不是
        // ReturnCode_t：有数据 true / 超时 false。别习惯性地拿它和
        // RETCODE_OK 比较 —— RETCODE_OK 是 0 而 true 是 1，那样会写成
        // "有数据时反而跳过"。
        if (!reader_->wait_for_unread_message(wait_for))
        {
            return false;
        }

        SampleInfo info;
        // take_next_sample() 取一条并从缓存里删掉它。
        if (RETCODE_OK != reader_->take_next_sample(&out, &info))
        {
            return false;
        }
        // valid_data 为 false 表示这是 dispose / unregister 之类的
        // 生命周期事件，不是真实数据。
        return info.valid_data;
    }

    // 等发现完成：等到至少有一个对端匹配上。
    // DDS 靠后台的发现协议自动找对端，这需要一点时间；
    // 不等就开始发，样本可能在还没有对端的情况下被丢掉。
    bool wait_for_match(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (reading_)
            {
                SubscriptionMatchedStatus status;
                reader_->get_subscription_matched_status(status);
                if (status.current_count > 0) return true;
            }
            else
            {
                PublicationMatchedStatus status;
                writer_->get_publication_matched_status(status);
                if (status.current_count > 0) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

private:
    DomainParticipant* participant_{};
    Topic* topic_{};
    Publisher* publisher_{};
    Subscriber* subscriber_{};
    DataWriter* writer_{};
    DataReader* reader_{};
    bool reading_{};
};

// ── 服务端 ─────────────────────────────────────────────────────────────────
// 对应 rclcpp 的 node->create_service(name, handler) + node->spin()。
// 内部两个端点：一个读请求，一个写响应。
class Service
{
public:
    // handler 收到请求，返回响应（不用自己填 request_id，spin 会补上）。
    using Handler = std::function<AddResponse(const AddRequest&)>;

    Service(const std::string& request_topic, const std::string& response_topic)
        : participant_(create_participant())
        , requests_(participant_, request_topic, true)     // true  = 收
        , responses_(participant_, response_topic, false)  // false = 发
    {
    }

    ~Service() { delete_participant(participant_); }

    // 等客户端出现。服务端先跑起来、客户端后跑，是更稳的顺序。
    bool wait_for_clients(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return requests_.wait_for_match(timeout);
    }

    // 阻塞循环：取请求 → 调用 handler → 写回响应。对应 rclcpp 的 spin()。
    void spin(const Handler& handler)
    {
        std::cout << "服务端就绪，等待请求...\n";
        AddRequest request;
        while (running_)
        {
            // 200ms 超时只是为了给循环一个检查 running_ 的机会，
            // 不然 Ctrl-C 之后要等下一个请求才会退出。
            if (!requests_.take(request, std::chrono::milliseconds(200)))
            {
                continue;
            }

            std::cout << "收到请求 #" << request.request_id()
                      << ": add(" << request.a() << ", " << request.b() << ")\n";

            AddResponse response = handler(request);
            // 【关键】把请求的 id 原样带回响应里，客户端靠它配对。
            // ROS 2 的真实服务由 RMW 按 sequence number 自动完成这件事。
            response.request_id(request.request_id());
            responses_.publish(response);
        }
    }

    void stop() { running_ = false; }

private:
    DomainParticipant* participant_{};
    Endpoint<AddRequest, AddRequestPubSubType> requests_;
    Endpoint<AddResponse, AddResponsePubSubType> responses_;
    std::atomic<bool> running_{true};
};

// ── 客户端 ─────────────────────────────────────────────────────────────────
// 对应 rclcpp 的 node->create_client(name) + async_send_request()。
// 同样是两个端点，但方向反过来：写请求、读响应。
class Client
{
public:
    Client(const std::string& request_topic, const std::string& response_topic)
        : participant_(create_participant())
        , requests_(participant_, request_topic, false)   // false = 发
        , responses_(participant_, response_topic, true)  // true  = 收
    {
    }

    ~Client() { delete_participant(participant_); }

    // 等服务端出现。不等就发，请求会在服务端还没匹配上时被丢掉，
    // 客户端只能等到超时 —— 这是 DDS 上做 RPC 最常见的坑。
    bool wait_for_service(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return requests_.wait_for_match(timeout) && responses_.wait_for_match(timeout);
    }

    // 同步调用：发请求 → 等 id 对得上的响应。返回 false 表示超时。
    // ROS 2 的 call() 是异步的（返回 future），这里为了讲清楚配对过程
    // 写成同步的。
    bool call(long a, long b, AddResponse& out,
              std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        AddRequest request;
        request.request_id(next_request_id_++);
        request.a(a);
        request.b(b);
        requests_.publish(request);

        // 只认 id 对得上的响应，对不上的丢掉继续等。
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        AddResponse response;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (!responses_.take(response, left))
            {
                continue;
            }
            if (response.request_id() == request.request_id())
            {
                out = response;
                return true;
            }
            // id 不匹配：不是给我的响应（可能是别的客户端请求触发的），丢弃。
        }
        return false;
    }

private:
    DomainParticipant* participant_{};
    Endpoint<AddRequest, AddRequestPubSubType> requests_;
    Endpoint<AddResponse, AddResponsePubSubType> responses_;
    unsigned long long next_request_id_{1};
};

}  // namespace lesson02
