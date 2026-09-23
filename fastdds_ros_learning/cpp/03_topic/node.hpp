#pragma once

// 第 3 章 · 把 DDS Topic 包成 ROS 2 风格的发布订阅
//
// ── ROS 2 里的写法 ─────────────────────────────────────────────────────────
//     auto node = std::make_shared<rclcpp::Node>("chatter");
//     auto pub  = node->create_publisher<Chatter>("topic", 10);
//     auto sub  = node->create_subscription<Chatter>("topic", 10, callback);
//     rclcpp::spin(node);
//
// ── 本章做的事 ─────────────────────────────────────────────────────────────
//   把 DDS 实体包成同样形状的类：
//     Node          持有一个 DomainParticipant
//     Publisher     包 DataWriter，对外只有 publish()
//     Subscription  包 DataReader，回调在 on_data_available() 里触发
//
// ── 三个必须知道的差异 ─────────────────────────────────────────────────────
//   1. 【spin() 的含义不同】ROS 2 的 spin() 是主线程去跑回调队列；DDS 的回调
//      跑在中间件自己的线程上，所以这里的 spin() 只是把主线程阻塞住，防止
//      main 提前退出。回调本身不需要 spin() 驱动。
//   2. ROS 2 有 namespace / remapping / 命名规则那一套；这里 Topic 名就是
//      DDS 上真正的名字，不经过任何改名。
//   3. QoS 直接暴露 DDS 的。ROS 2 的 QoS 是它自己的一套语义，最终由 RMW
//      映射到 DDS QoS；这里不做那层映射。

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "TopicTypes.hpp"
#include "TopicTypesPubSubTypes.hpp"

namespace lesson03 {

using namespace eprosima::fastdds::dds;

// 【注意命名冲突】本命名空间里定义的 Publisher / Subscription 是 ROS 风格的
// 封装类，而 eprosima::fastdds::dds 里有一个同名的 Publisher —— 那是 DDS 的
// "发布侧容器"实体，两者完全不是一回事。因为上面有 using namespace，
// 在 lesson03 内部裸写 Publisher 会优先解析到我们自己的类。
// 所以下面凡是提到 DDS 的那个，都写全限定名 eprosima::fastdds::dds::Publisher。

// 订阅回调：收到一条就给一次。对应 rclcpp 里 lambda 形式的 callback。
using Callback = std::function<void(const Chatter&)>;

// ── 发布者 ─────────────────────────────────────────────────────────────────
// 对应 rclcpp 的 rclcpp::Publisher<Chatter>。
// 对外只有 publish()，把 DataWriter 藏起来。
class Publisher
{
public:
    Publisher(DataWriter* writer) : writer_(writer) {}

    // 对应 rclcpp 的 publisher->publish(msg)。
    // write() 收的是 const void*，直接传 const 引用取地址即可。
    // 返回后不代表对端已经收到 —— DDS 是异步的。
    void publish(const Chatter& msg)
    {
        writer_->write(&msg);
    }

    // 等至少有一个订阅者匹配上（对应 ROS 2 里 publisher 的订阅计数 > 0）。
    bool wait_for_subscription(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            PublicationMatchedStatus status;
            writer_->get_publication_matched_status(status);
            if (status.current_count > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

private:
    DataWriter* writer_{};
};

// ── 订阅者 ─────────────────────────────────────────────────────────────────
// 对应 rclcpp 的 rclcpp::Subscription<Chatter>。
//
// 它【同时是】DDS 的 DataReaderListener：create_datareader() 时把 this 传进去，
// 数据到了 Fast DDS 就在自己的内部线程上调 on_data_available()，
// 我们把样本取出来交给用户回调。
//
// 回调运行在中间件线程上，所以：
//   * 不要在回调里做长时间阻塞的操作，会拖住 DDS 的收发线程；
//   * 回调里碰到的共享数据要自己加锁。
class Subscription : public DataReaderListener
{
public:
    explicit Subscription(Callback callback) : callback_(std::move(callback)) {}

    void on_data_available(DataReader* reader) override
    {
        Chatter sample;
        SampleInfo info;
        // 必须 while 排空：回调只保证"此刻有数据"，不保证只有一条。
        while (RETCODE_OK == reader->take_next_sample(&sample, &info))
        {
            if (!info.valid_data)   // dispose / unregister 等生命周期事件，不是数据
            {
                continue;
            }
            callback_(sample);
        }
    }

    bool wait_for_publisher(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        if (nullptr == reader_)
        {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            SubscriptionMatchedStatus status;
            reader_->get_subscription_matched_status(status);
            if (status.current_count > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

private:
    friend class Node;
    DataReader* reader_{};
    Callback callback_;
};

// ── 节点 ───────────────────────────────────────────────────────────────────
// 对应 rclcpp::Node。持有一个 DomainParticipant，
// 以及这个参与者下面的 Topic / Publisher / Subscriber 实体。
class Node
{
public:
    explicit Node(const std::string& name)
        : name_(name)
    {
        // ① 工厂 → 参与者。域号相同的进程才能互相发现，本章都用域 0。
        DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();
        participant_ = factory->create_participant(0, PARTICIPANT_QOS_DEFAULT);

        // ② 注册类型。一个节点里同一种类型注册一次就够，
        //    重复注册同名类型会返回 RETCODE_PRECONDITION_NOT_MET。
        //    这一步必须在 create_topic 之前。
        type_ = TypeSupport(new ChatterPubSubType());
        participant_->register_type(type_);
    }

    ~Node()
    {
        if (nullptr != participant_)
        {
            // delete_contained_entities() 会连带删掉这个参与者下面的
            // Topic / Publisher / Subscriber / DataWriter / DataReader。
            participant_->delete_contained_entities();
            DomainParticipantFactory::get_instance()->delete_participant(participant_);
        }
    }

    const std::string& name() const { return name_; }

    // 对应 rclcpp 的 node->create_publisher<Chatter>(topic, qos_depth)。
    // 返回 shared_ptr 而不是值 —— Subscription 要作为 listener 被 DDS 长期
    // 持有指针，拷贝/移动会让那个指针失效。用 shared_ptr 表达"这个对象
    // 由节点和调用方共同持有"。
    std::shared_ptr<Publisher> create_publisher(
        const std::string& topic_name, uint32_t history_depth = 10)
    {
        // ③ 建 Topic：名字 + 类型。这一步必须在 register_type 之后。
        Topic* topic = participant_->create_topic(
            topic_name, type_.get_type_name(), TOPIC_QOS_DEFAULT);

        // ④ Publisher 是发布侧的容器，一个节点共用一个就够。
        //    这里要写全限定名 —— 裸写 Publisher 会指到本文件的封装类上去。
        if (nullptr == publisher_)
        {
            publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
        }

        // ⑤ 建 DataWriter。想改 QoS 的顺序是「取默认值 → 改字段 → 再创建」：
        //    get_default_datawriter_qos() 是【整体赋值】而不是合并字段，
        //    顺序写反的话你设的值会被它覆盖掉。
        DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
        publisher_->get_default_datawriter_qos(qos);
        qos.history().depth = history_depth;   // 每个实例在本地缓存多少条历史

        DataWriter* writer = publisher_->create_datawriter(topic, qos);

        auto pub = std::make_shared<Publisher>(writer);
        publishers_.push_back(pub);   // 由节点保活，见类注释
        return pub;
    }

    // 对应 rclcpp 的 node->create_subscription<Chatter>(topic, qos_depth, callback)。
    std::shared_ptr<Subscription> create_subscription(
        const std::string& topic_name, Callback callback, uint32_t history_depth = 10)
    {
        Topic* topic = participant_->create_topic(
            topic_name, type_.get_type_name(), TOPIC_QOS_DEFAULT);

        if (nullptr == subscriber_)
        {
            subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        }

        auto sub = std::make_shared<Subscription>(std::move(callback));

        DataReaderQos qos = DATAREADER_QOS_DEFAULT;
        subscriber_->get_default_datareader_qos(qos);
        qos.history().depth = history_depth;

        // ⑥ 建 DataReader，并把 sub 自己作为监听器传进去 ——
        //    这是"数据到了回调我"和"主线程主动轮询"的分水岭。
        //    Subscription 的生命周期必须覆盖 DataReader，所以节点也要持有它。
        sub->reader_ = subscriber_->create_datareader(topic, qos, sub.get());

        subscriptions_.push_back(sub);
        return sub;
    }

    // 对应 rclcpp::spin(node)。
    // 【注意】这里只是阻塞主线程，回调并不需要它来驱动 ——
    // 回调跑在 DDS 内部线程上。见文件开头的差异说明。
    void spin()
    {
        while (running_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void shutdown() { running_ = false; }

private:
    std::string name_;
    DomainParticipant* participant_{};
    // 全限定名：这是我们封装类的 DDS 对应物，别和 lesson03::Publisher 搞混。
    eprosima::fastdds::dds::Publisher* publisher_{};
    Subscriber* subscriber_{};
    TypeSupport type_;

    // 节点持有所有创建过的端点，保证它们的生命周期不短于节点。
    // 尤其是 Subscription：它作为 listener 被 DataReader 持有裸指针，
    // 提前析构会导致 DDS 线程回调到已释放的内存。
    std::vector<std::shared_ptr<Publisher>> publishers_;
    std::vector<std::shared_ptr<Subscription>> subscriptions_;

    std::atomic<bool> running_{true};
};

}  // namespace lesson03
