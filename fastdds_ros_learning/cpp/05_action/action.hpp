#pragma once

// 第 5 章 · 用 DDS 实现 ROS 2 风格的 action
//
// ── ROS 2 里的 action 长什么样 ─────────────────────────────────────────────
//     rclcpp_action::create_server<Fibonacci>(node, "fibonacci",
//         handle_goal, handle_cancel, handle_accepted);
//     rclcpp_action::create_client<Fibonacci>(node, "fibonacci");
//
//   action 是"有状态的长任务"：客户端发一个目标，服务端边跑边报进度，
//   跑完给结果，中途还能取消。它比服务多出两件事：
//     * 反馈通道（单向、多次）
//     * 取消通道（反向、随时）
//
// ── DDS 里怎么拼出来 ───────────────────────────────────────────────────────
//   四条 Topic，靠 goal_id 关联：
//
//     客户端 --FibonacciGoal------> /demo/fibonacci/goal      --> 服务端
//     客户端 <--FibonacciFeedback-- /demo/fibonacci/feedback  <-- 服务端
//     客户端 <--FibonacciResult---- /demo/fibonacci/result    <-- 服务端
//     客户端 --FibonacciCancel----> /demo/fibonacci/cancel    --> 服务端
//
//   和服务的最大区别：服务一次请求一个响应就结束了，action 的会话是
//   【长时间存活】的 —— 服务端要一边算一边发反馈，同时还要盯着取消通道。
//
// ── 从服务到 action：多了两条通道 ──────────────────────────────────────────
//
//    ┌─ 第 2 章·服务（2 条通道）──┐   ┌─ 第 5 章·action（4 条通道）──────┐
//    │  request  ──request_id──┐ │   │  goal      ──goal_id──┐          │
//    │  response ──request_id──┘ │   │  feedback  ──goal_id──┤          │
//    │                           │   │  result    ──goal_id──┤ 靠 id 关联│
//    │  一问一答，完事就结束       │   │  cancel    ──goal_id──┘          │
//    │                           │   │  feedback 单向、可多次            │
//    │                           │   │  cancel   反向、随时可发          │
//    └───────────────────────────┘   └──────────────────────────────────┘
//
// ── 一次执行的时间线（正常跑完）─────────────────────────────────────────────
//
//   Client                                    Server
//     │ ① send_goal(order=8)                   │
//     │ ──────── /goal ──────────────────────> │ ② goals_.take()
//     │                                        │ ③ execute(handle) 开始
//     │ ④ on_feedback() 被回调                  │    ┌─ 循环 i=0..7 ─┐
//     │ <─────── /feedback (i=0, val=0) ────── │    │ ⑤ is_canceling()
//     │ <─────── /feedback (i=1, val=1) ────── │    │    没有 → 继续
//     │ <─────── /feedback (i=2, val=1) ────── │    │ ⑥ publish_feedback
//     │           ...                          │    │ ⑦ sleep 500ms
//     │ <─────── /feedback (i=7, val=13) ───── │    └────────────────┘
//     │                                        │ ⑧ 循环结束，填 result
//     │ <─────── /result ──────────────────── │ ⑨ results_.publish()
//     │    goal_id 对得上 → 打印 "已完成"        │
//
//   反馈和结果走的是【两条不同的 Topic】，客户端要同时盯两个 reader。
//
// ── 取消路径：这是和服务最本质的区别 ───────────────────────────────────────
//
//   Client                                    Server
//     │ <─────── feedback (i=0..2) ─────────── │    ┌─ 循环 ─┐
//     │                                        │    │        │
//     │ ③ 决定取消，cancel_goal(goal_id=1)      │    │ ④ 下一轮
//     │ ──────── /cancel ────────────────────> │    │  is_canceling()
//     │                                        │    │  ↓ try_take 到
//     │                                        │    │  cancel.goal_id==1
//     │                                        │    │  → 返回 true
//     │                                        │    │ ⑤ 跳出循环
//     │                                        │    │    canceled = true
//     │ <─────── /result ──────────────────── │    └────────┘
//     │    canceled = true → 打印 "已取消"       │ ⑥ publish 结果
//
//   第 ④ 步是核心：服务端【每一轮循环】都主动回头看一眼取消通道。
//   因为 is_canceling() 用的是非阻塞的 try_take()，这一眼不会把执行卡住。
//
//   ⚠ 如果误用阻塞版 take()，服务端会卡在"等取消"上 —— 任务既算不完，
//     也永远等不到取消（取消只在客户端想取消时才发）。
//
// ── GoalHandle 的作用 ──────────────────────────────────────────────────────
//
//   ActionServer::spin(execute)
//     │ ① take 到 goal
//     │ ② 构造 GoalHandle(goal, &feedbacks_, &cancels_)
//     │         ├── 持有 goal 本身
//     │         ├── 持有 feedback 端点（发反馈）
//     │         └── 持有 cancel 端点（查取消）
//     │ ③ execute(handle)  ← 交给你的回调
//     │         └─ 执行代码里通过 handle 报进度、查取消
//     │ ④ 拿返回值，回填 goal_id，publish 结果
//     ▼ 回到 ①
//
//   把两个端点都塞进 handle，而不是让用户直接摸 ActionServer 的成员 ——
//   因为执行回调需要的只是"跟这个目标相关的那两条通道"。
//   这也是 rclcpp_action 里 ServerGoalHandle 的设计意图。
//
// ── 本文件的结构 ───────────────────────────────────────────────────────────
//   1. Endpoint<>    —— 最小的话题端点，全章唯一直接碰 DDS 的地方
//   2. GoalHandle    —— 执行期间交给用户回调的句柄，对应 rclcpp_action 的
//                       ServerGoalHandle（报反馈 / 查取消都通过它）
//   3. ActionServer  —— 服务端
//   4. ActionClient  —— 客户端
//
// 与 cpp/02_service/service.hpp、cpp/04_parameter/parameter.hpp 结构相同，
// 可以对照着看：request_id 换成了 goal_id，通道从 2 条变成 4 条。

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

#include "ActionTypes.hpp"
#include "ActionTypesPubSubTypes.hpp"

namespace lesson05 {

using namespace eprosima::fastdds::dds;

// 时间转换：Duration_t 内部是（int32 秒, uint32 纳秒），
// 直接用毫秒乘 1000000 塞第二个参数会溢出。
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
        //    【必须】在 register_type 之后。
        topic_ = participant->create_topic(
            topic_name, type.get_type_name(), TOPIC_QOS_DEFAULT);

        if (reading_)
        {
            // ③ 订阅侧。监听器传 nullptr → 主动轮询，不用回调。
            subscriber_ = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
            reader_ = subscriber_->create_datareader(topic_, DATAREADER_QOS_DEFAULT, nullptr);
        }
        else
        {
            // ③ 发布侧。
            publisher_ = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
            writer_ = publisher_->create_datawriter(topic_, DATAWRITER_QOS_DEFAULT);
        }
    }

    void publish(const SampleT& sample) { writer_->write(&sample); }

    // 阻塞等待最多 timeout，取出一条数据。返回 false 表示超时或只有生命周期事件。
    // 注意 wait_for_unread_message 返回 bool（有数据 true / 超时 false），
    // 不是 ReturnCode_t —— 别拿它和 RETCODE_OK 比较。
    bool take(SampleT& out, std::chrono::milliseconds timeout)
    {
        if (!reader_->wait_for_unread_message(to_duration(timeout)))
        {
            return false;
        }
        return take_now(out);
    }

    // 非阻塞取一条：有就取走，没有立刻返回 false。
    // 取消通道要用它 —— 检查取消不能把执行流程阻塞住。
    // take_next_sample() 本身就不阻塞，所以这里不需要等。
    bool try_take(SampleT& out) { return take_now(out); }

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
    bool take_now(SampleT& out)
    {
        SampleInfo info;
        if (RETCODE_OK != reader_->take_next_sample(&out, &info))
        {
            return false;
        }
        return info.valid_data;
    }

    Topic* topic_{};
    Publisher* publisher_{};
    Subscriber* subscriber_{};
    DataWriter* writer_{};
    DataReader* reader_{};
    bool reading_{};
};

// ── 目标句柄 ───────────────────────────────────────────────────────────────
// 执行期间交给用户回调，对应 rclcpp_action 的 ServerGoalHandle：
//   publish_feedback()  ←→ goal_handle->publish_feedback(feedback)
//   is_canceling()      ←→ goal_handle->is_canceling()
//   goal()              ←→ goal_handle->get_goal()
class GoalHandle
{
public:
    GoalHandle(const FibonacciGoal& goal,
               Endpoint<FibonacciFeedback, FibonacciFeedbackPubSubType>* feedback,
               Endpoint<FibonacciCancel, FibonacciCancelPubSubType>* cancel)
        : goal_(goal), feedback_(feedback), cancel_(cancel)
    {
    }

    const FibonacciGoal& goal() const { return goal_; }

    // 对应 goal_handle->publish_feedback()。执行途中可以调任意多次。
    void publish_feedback(long sequence_index, long partial_sequence)
    {
        FibonacciFeedback fb;
        fb.goal_id(goal_.goal_id());
        fb.sequence_index(sequence_index);
        fb.partial_sequence(partial_sequence);
        feedback_->publish(fb);
    }

    // 对应 goal_handle->is_canceling()。
    // 用非阻塞取数把取消通道排空 —— 检查取消不能把执行流程卡住。
    //
    // 注意这里是【非阻塞】的：没有取消请求时立刻返回 false，
    // 所以可以放在执行循环的每一轮里调用。
    bool is_canceling()
    {
        FibonacciCancel cancel;
        while (cancel_->try_take(cancel))
        {
            if (cancel.goal_id() == goal_.goal_id())
            {
                return true;
            }
            // 取消的是别的目标：本示例一次只处理一个目标，直接丢弃。
        }
        return false;
    }

private:
    FibonacciGoal goal_;
    Endpoint<FibonacciFeedback, FibonacciFeedbackPubSubType>* feedback_;
    Endpoint<FibonacciCancel, FibonacciCancelPubSubType>* cancel_;
};

// ── action 服务端 ──────────────────────────────────────────────────────────
class ActionServer
{
public:
    // 对应 rclcpp_action 的 execute_callback：在这里跑长任务，
    // 通过 handle 报反馈、查是否被取消，最后返回结果。
    using Execute = std::function<FibonacciResult(GoalHandle&)>;

    explicit ActionServer(const std::string& root = "/demo/fibonacci")
        : participant_(create_participant())
        , goals_(participant_, root + "/goal", true)          // 收目标
        , feedbacks_(participant_, root + "/feedback", false) // 发反馈
        , results_(participant_, root + "/result", false)     // 发结果
        , cancels_(participant_, root + "/cancel", true)      // 收取消
    {
    }

    ~ActionServer() { delete_participant(participant_); }

    bool wait_for_clients(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        // 四条通道都要匹配上，缺一条都会导致协议走不通。
        return goals_.wait_for_match(timeout)
            && feedbacks_.wait_for_match(timeout)
            && results_.wait_for_match(timeout)
            && cancels_.wait_for_match(timeout);
    }

    // 阻塞循环：取目标 → 执行 → 发结果。一次处理一个目标。
    void spin(const Execute& execute)
    {
        std::cout << "action 服务端就绪，等待目标...\n";
        FibonacciGoal goal;
        while (running_)
        {
            if (!goals_.take(goal, std::chrono::milliseconds(200)))
            {
                continue;
            }

            std::cout << "收到目标 #" << goal.goal_id()
                      << "：生成 " << goal.order() << " 个斐波那契数\n";

            GoalHandle handle(goal, &feedbacks_, &cancels_);
            FibonacciResult result = execute(handle);

            // 目标 id 由服务端保证回填，用户回调不用操心。
            result.goal_id(goal.goal_id());
            results_.publish(result);

            std::cout << "目标 #" << goal.goal_id()
                      << (result.canceled() ? " 已取消" : " 已完成")
                      << "，结果 " << result.result() << "\n";
        }
    }

    void stop() { running_ = false; }

private:
    DomainParticipant* participant_{};
    Endpoint<FibonacciGoal, FibonacciGoalPubSubType> goals_;
    Endpoint<FibonacciFeedback, FibonacciFeedbackPubSubType> feedbacks_;
    Endpoint<FibonacciResult, FibonacciResultPubSubType> results_;
    Endpoint<FibonacciCancel, FibonacciCancelPubSubType> cancels_;
    std::atomic<bool> running_{true};
};

// ── action 客户端 ──────────────────────────────────────────────────────────
class ActionClient
{
public:
    // 反馈回调：服务端每报一次进度就调一次。
    using FeedbackCallback = std::function<void(const FibonacciFeedback&)>;

    explicit ActionClient(const std::string& root = "/demo/fibonacci")
        : participant_(create_participant())
        , goals_(participant_, root + "/goal", false)         // 发目标
        , feedbacks_(participant_, root + "/feedback", true)  // 收反馈
        , results_(participant_, root + "/result", true)      // 收结果
        , cancels_(participant_, root + "/cancel", false)     // 发取消
    {
    }

    ~ActionClient() { delete_participant(participant_); }

    bool wait_for_server(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return goals_.wait_for_match(timeout)
            && feedbacks_.wait_for_match(timeout)
            && results_.wait_for_match(timeout)
            && cancels_.wait_for_match(timeout);
    }

    // 对应 rclcpp_action 的 async_send_goal()。返回目标 id。
    unsigned long long send_goal(long order)
    {
        FibonacciGoal goal;
        goal.goal_id(next_goal_id_++);
        goal.order(order);
        goals_.publish(goal);
        return goal.goal_id();
    }

    // 对应 async_cancel_goal()。
    void cancel_goal(unsigned long long goal_id)
    {
        FibonacciCancel cancel;
        cancel.goal_id(goal_id);
        cancels_.publish(cancel);
    }

    // 对应 async_get_result() 的 future.wait()。
    // 一边等结果，一边把收到的反馈交给回调。
    bool wait_for_result(unsigned long long goal_id, FibonacciResult& out,
                         FeedbackCallback on_feedback = nullptr,
                         std::chrono::milliseconds timeout = std::chrono::seconds(30))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

            // 反馈可能来好几条，所以先尽量排空反馈通道。
            FibonacciFeedback feedback;
            while (feedbacks_.try_take(feedback))
            {
                if (on_feedback && feedback.goal_id() == goal_id)
                {
                    on_feedback(feedback);
                }
            }

            // 再阻塞等一会儿结果。
            FibonacciResult result;
            if (results_.take(result, std::min(left, std::chrono::milliseconds(200))))
            {
                if (result.goal_id() == goal_id)
                {
                    out = result;
                    return true;
                }
                // 不是这个目标的结果，丢弃继续等。
            }
        }
        return false;
    }

private:
    DomainParticipant* participant_{};
    Endpoint<FibonacciGoal, FibonacciGoalPubSubType> goals_;
    Endpoint<FibonacciFeedback, FibonacciFeedbackPubSubType> feedbacks_;
    Endpoint<FibonacciResult, FibonacciResultPubSubType> results_;
    Endpoint<FibonacciCancel, FibonacciCancelPubSubType> cancels_;
    unsigned long long next_goal_id_{1};
};

}  // namespace lesson05
