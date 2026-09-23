#pragma once

// 第 4 章 · 用 DDS 实现 ROS 2 风格的参数系统
//
// ── ROS 2 里的参数长什么样 ─────────────────────────────────────────────────
//     node->declare_parameter("speed", 1);      // 声明，带默认值
//     node->get_parameter("speed");             // 读
//     node->set_parameter({"speed", 5});        // 写
//     node->add_on_set_parameters_callback(cb); // 参数被改时收到通知
//
// ── DDS 里怎么拼出来 ───────────────────────────────────────────────────────
//   参数系统是"请求-响应"（和第 2 章的服务同构），外加一条单向的通知通道：
//
//     客户端 --ParamRequest-->  /demo/parameters/request  --> 服务端
//     客户端 <--ParamResponse-- /demo/parameters/response <-- 服务端
//     所有人 <--ParamEvent----- /demo/parameter_events    <-- 服务端
//
//   第三条 Topic 就是 ROS 2 里的 parameter_events：它不是谁请求的响应，
//   而是"参数变了"的广播。任何进程订阅它都能感知到参数变化。
//
// ── 和第 2 章的关系：服务骨架 + 一条广播通道 ────────────────────────────────
//
//    ┌─ 第 2 章·服务 ─────────────────┐   ┌─ 第 4 章·参数 ────────────────┐
//    │  请求 Topic  ──request_id──┐   │   │  请求 Topic  ──request_id──┐ │
//    │  响应 Topic  ──request_id──┘   │   │  响应 Topic  ──request_id──┘ │
//    │        ↑ 靠 id 配对            │   │        ↑ 靠 id 配对          │
//    │                                │   │  参数事件 Topic  ← 新增！     │
//    │                                │   │    纯广播，没有配对           │
//    └────────────────────────────────┘   └───────────────────────────────┘
//
//   Service/Client 里那一套 Endpoint + request_id 配对，参数系统原样照搬；
//   多出来的只有一条单向的 ParamEvent writer。
//
// ── 为什么事件要单独占一条 Topic ───────────────────────────────────────────
//
//   ✗ 塞进响应里：响应只回给【发起修改的那个客户端】
//
//       客户端 A ──set──> 服务端 ──response──> 客户端 A   ← 只有 A 知道
//                                           （B、C 什么都没收到）
//
//   ✓ 单独开一条广播 Topic：服务端只写一次，DDS 复制给所有订阅者
//
//       客户端 A ──set──> 服务端
//                           ├──response──> 客户端 A       ← 谁问的给谁
//                           │
//                           └──ParamEvent─> /demo/parameter_events
//                                             ├──> 监听进程 monitor
//                                             ├──> 客户端 B（如果订了）
//                                             └──> 客户端 C（如果订了）
//
//   参数变化是【需要广播出去的全局事件】，不是某个请求的私有响应。
//   两者语义不同，所以在 DDS 上必须占两条不同的 Topic。
//   这就是 ROS 2 里 parameter_events 话题存在的理由。
//
// ── 参数表在哪里 ───────────────────────────────────────────────────────────
//
//   ┌─ 服务端进程 ─────────────────────────────────────────┐
//   │   std::map<std::string, long> parameters_;  ← 就是它  │
//   │        "speed" → 5                                  │
//   │   普通的应用层数据结构，和 DDS 没有任何关系            │
//   └─────────────────────────────────────────────────────┘
//                            ▲
//          DDS 提供的只是「传输」这一层：
//                            │
//   ┌────────────────────────┴────────────────────────────┐
//   │  序列化 → RTPS → 网络 → 反序列化 → 放进对端缓存        │
//   └─────────────────────────────────────────────────────┘
//
//   参数的【语义】全在你自己的代码里 —— 什么算"已声明"、读未声明的
//   参数怎么办、改了要不要通知，都是你定义的。ROS 2 规定了这些语义，
//   本章只是模仿它。
//
// ── 本文件的结构 ───────────────────────────────────────────────────────────
//   1. Endpoint<>        —— 最小的话题端点，全章唯一直接碰 DDS 的地方
//   2. ParameterServer   —— 服务端：声明/读取/修改 + 发事件
//   3. ParameterClient   —— 客户端：declare / get / set
//
// 与 cpp/02_service/service.hpp 结构相同，可以对照着看 ——
// 参数和服务的 DDS 骨架是一模一样的，差别只在多了一条事件 Topic。

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
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

#include "ParameterTypes.hpp"
#include "ParameterTypesPubSubTypes.hpp"

namespace lesson04 {

using namespace eprosima::fastdds::dds;

// 时间转换：Duration_t 内部是（int32 秒, uint32 纳秒），
// 直接用毫秒乘 1000000 塞第二个参数会溢出（5 秒 = 5e9 纳秒 > uint32 上限）。
inline Duration_t to_duration(std::chrono::milliseconds timeout)
{
    return Duration_t(static_cast<long double>(timeout.count()) / 1000.0L);
}

inline DomainParticipant* create_participant(int domain_id = 0)
{
    DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();
    return factory->create_participant(domain_id, PARTICIPANT_QOS_DEFAULT);
}

inline void delete_participant(DomainParticipant* participant)
{
    if (nullptr == participant) return;
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
}

// ── 端点：一个话题上的"发"或"收" ───────────────────────────────────────────
// 模板参数是 fastddsgen 生成的两个类：
//   SampleT      —— 数据结构，例如 ParamRequest
//   PubSubTypeT  —— DDS 类型适配类，例如 ParamRequestPubSubType
template <class SampleT, class PubSubTypeT>
class Endpoint
{
public:
    Endpoint(DomainParticipant* participant, const std::string& topic_name, bool reading)
        : reading_(reading)
    {
        // ① 注册类型：把生成的数据结构和 DDS 中间件关联起来。
        TypeSupport type(new PubSubTypeT());
        participant->register_type(type);

        // ② 建 Topic：把一个【名字】和一个【类型】绑在一起。
        //    【必须】在 register_type 之后 —— create_topic 会去查找已注册的
        //    类型，找不到就返回 nullptr。
        topic_ = participant->create_topic(
            topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);

        if (reading_)
        {
            // ③ 订阅侧：Subscriber 是容器，DataReader 才是真正收数据的实体。
            //    监听器传 nullptr → 不用回调，改成主动轮询（见 take()）。
            subscriber_ = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
            reader_ = subscriber_->create_datareader(topic_, DATAREADER_QOS_DEFAULT, nullptr);
        }
        else
        {
            // ③ 发布侧：Publisher 是容器，DataWriter 才是真正发数据的实体。
            publisher_ = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
            writer_ = publisher_->create_datawriter(topic_, DATAWRITER_QOS_DEFAULT);
        }
    }

    void publish(const SampleT& sample) { writer_->write(&sample); }

    // 阻塞等待最多 timeout，取出一条数据。返回 false 表示超时或只有生命周期事件。
    bool take(SampleT& out, std::chrono::milliseconds timeout)
    {
        // 注意：wait_for_unread_message 返回 bool（有数据 true / 超时 false），
        // 不是 ReturnCode_t，别拿它和 RETCODE_OK 比较。
        if (!reader_->wait_for_unread_message(to_duration(timeout)))
        {
            return false;
        }
        SampleInfo info;
        if (RETCODE_OK != reader_->take_next_sample(&out, &info))
        {
            return false;
        }
        return info.valid_data;
    }

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
    Topic* topic_{};
    Publisher* publisher_{};
    Subscriber* subscriber_{};
    DataWriter* writer_{};
    DataReader* reader_{};
    bool reading_{};
};

// ── 参数服务端 ─────────────────────────────────────────────────────────────
// 对应 rclcpp 里带参数的那个 Node（declare_parameter / get_parameter /
// set_parameter / parameter_events）。
class ParameterServer
{
public:
    explicit ParameterServer(
        const std::string& request_topic = "/demo/parameters/request",
        const std::string& response_topic = "/demo/parameters/response",
        const std::string& events_topic = "/demo/parameter_events")
        : participant_(create_participant())
        , requests_(participant_, request_topic, true)     // 收请求
        , responses_(participant_, response_topic, false)  // 发响应
        , events_(participant_, events_topic, false)       // 发参数事件
    {
    }

    ~ParameterServer() { delete_participant(participant_); }

    bool wait_for_clients(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return requests_.wait_for_match(timeout);
    }

    // 阻塞循环：取请求 → 处理 → 回响应（修改操作额外补一条事件）。
    void spin()
    {
        ParamRequest request;
        while (running_)
        {
            // 200ms 超时只是为了给循环一个检查 running_ 的机会。
            if (!requests_.take(request, std::chrono::milliseconds(200)))
            {
                continue;
            }

            ParamResponse response;
            response.request_id(request.request_id());
            response.name(request.name());
            response.success(true);

            switch (request.op())
            {
                case ParamOp::PARAM_DECLARE:
                    // 声明：已经有了就保留原值（对应 ROS 2 里重复声明会报错，
                    // 这里宽松处理，用 setdefault 的语义）。
                    parameters_.emplace(request.name(), request.value());
                    response.value(parameters_[request.name()]);
                    std::cout << "声明参数 " << request.name()
                              << " = " << response.value() << "\n";
                    break;

                case ParamOp::PARAM_GET:
                {
                    auto it = parameters_.find(request.name());
                    if (it == parameters_.end())
                    {
                        // 读一个没声明过的参数：如实返回失败，
                        // 而不是悄悄给个 0 —— 后者会让调用方以为参数存在。
                        response.success(false);
                        response.value(0);
                        std::cout << "读取失败：参数 " << request.name() << " 未声明\n";
                    }
                    else
                    {
                        response.value(it->second);
                        std::cout << "读取参数 " << request.name()
                                  << " = " << it->second << "\n";
                    }
                    break;
                }

                case ParamOp::PARAM_SET:
                {
                    auto it = parameters_.find(request.name());
                    if (it == parameters_.end())
                    {
                        response.success(false);
                        response.value(0);
                        std::cout << "修改失败：参数 " << request.name() << " 未声明\n";
                    }
                    else
                    {
                        it->second = request.value();
                        response.value(it->second);
                        std::cout << "修改参数 " << request.name()
                                  << " = " << it->second << "\n";

                        // 【和服务的区别】改参数额外广播一条事件。
                        // 这条不是给请求方的响应，而是给所有订阅者的通知。
                        ParamEvent event;
                        event.name(request.name());
                        event.value(it->second);
                        events_.publish(event);
                    }
                    break;
                }
            }

            responses_.publish(response);
        }
    }

    void stop() { running_ = false; }

private:
    DomainParticipant* participant_{};
    Endpoint<ParamRequest, ParamRequestPubSubType> requests_;
    Endpoint<ParamResponse, ParamResponsePubSubType> responses_;
    Endpoint<ParamEvent, ParamEventPubSubType> events_;
    std::map<std::string, long> parameters_;   // 参数表：名字 → 当前值
    std::atomic<bool> running_{true};
};

// ── 参数客户端 ─────────────────────────────────────────────────────────────
// 对应 rclcpp 的 node->declare_parameter / get_parameter / set_parameter。
class ParameterClient
{
public:
    explicit ParameterClient(
        const std::string& request_topic = "/demo/parameters/request",
        const std::string& response_topic = "/demo/parameters/response")
        : participant_(create_participant())
        , requests_(participant_, request_topic, false)   // 发请求
        , responses_(participant_, response_topic, true)  // 收响应
    {
    }

    ~ParameterClient() { delete_participant(participant_); }

    bool wait_for_server(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return requests_.wait_for_match(timeout) && responses_.wait_for_match(timeout);
    }

    // 声明参数。对应 rclcpp 的 declare_parameter(name, default_value)。
    bool declare_parameter(const std::string& name, long default_value, long& out)
    {
        return request(ParamOp::PARAM_DECLARE, name, default_value, out);
    }

    // 读取参数。对应 rclcpp 的 get_parameter(name).as_int()。
    bool get_parameter(const std::string& name, long& out)
    {
        return request(ParamOp::PARAM_GET, name, 0, out);
    }

    // 修改参数。对应 rclcpp 的 set_parameter({"name", value})。
    bool set_parameter(const std::string& name, long value)
    {
        long ignored = 0;
        return request(ParamOp::PARAM_SET, name, value, ignored);
    }

private:
    bool request(ParamOp op, const std::string& name, long value, long& out,
                 std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        ParamRequest request;
        request.request_id(next_request_id_++);
        request.op(op);
        request.name(name);
        request.value(value);
        requests_.publish(request);

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        ParamResponse response;
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
                out = response.value();
                return response.success();
            }
            // id 不匹配：不是给我的响应，丢弃。
        }
        return false;
    }

    DomainParticipant* participant_{};
    Endpoint<ParamRequest, ParamRequestPubSubType> requests_;
    Endpoint<ParamResponse, ParamResponsePubSubType> responses_;
    unsigned long long next_request_id_{1};
};

}  // namespace lesson04
