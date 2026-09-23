// 第 1 章 · Fast DDS 裸 API —— 发布端
//
// 这个文件不用任何自定义封装，完整走一遍 Fast DDS 实体的创建流程。
//
// ── 实体包含关系 ───────────────────────────────────────────────────────────
// Fast DDS 的对象是一棵【包含关系】的树，创建顺序有硬性依赖：
//
//   DomainParticipantFactory        进程内单例，一切的入口
//     └── DomainParticipant         参与者：本进程在某个 DDS 域里的身份
//           ├── TypeSupport         类型：告诉中间件怎么把 HelloWorld 变成字节
//           └── Topic               话题：把一个【名字】和一个【类型】绑在一起
//                 └── Publisher          发布侧的容器（订阅侧对应 Subscriber）
//                       └── DataWriter   真正执行发送的实体
//
// ── main() 的八步，和下面注释里的编号一一对应 ──────────────────────────────
//
//    ① factory       = DomainParticipantFactory::get_instance()
//         │
//    ② participant   = factory->create_participant(0, ...)        域号=0
//         │
//    ③ type.register_type(participant)      ⚠ 必须早于第 ④ 步
//         │
//    ④ topic         = participant->create_topic(名字, 类型名, ...)
//         │
//    ⑤ publisher     = participant->create_publisher(...)
//         │  writer    = publisher->create_datawriter(topic, qos)
//         │
//    ⑥ 等 matched.current_count > 0         ⚠ 不等就可能丢样本
//         │      ▲ 这一步等的是【后台发现协议】把两端撮合好
//         │      │
//    ⑦ writer->write(&sample) × 10
//         │
//    ⑧ participant->delete_contained_entities()
//       factory->delete_participant(participant)
//
// ── write() 之后数据经历了什么 ─────────────────────────────────────────────
// write() 返回 RETCODE_OK 只代表【中间件收下了】，对端收到与否是另一回事：
//
//    write(&sample)
//         │ ① 按 IDL 定义序列化成字节流（HelloWorldPubSubType 干的）
//         ▼
//    DataWriter 历史缓存   ② history().depth 决定存几条，可靠传输留到对端确认
//         │
//         │ ③ RTPS 协议分片、编号、必要时重传
//         ▼
//    ═══════════════════ 网络 / 共享内存 ═══════════════════
//         │
//         ▼
//    对端 DataReader 缓存   ④ 通知 reader：有新数据了
//         │
//         ▼
//    对端 take_next_sample()
//
// 所以这是个【异步】过程。真要确认对端收到了，得调
// writer->wait_for_acknowledgments()。
//
// 三条必须记住的顺序约束：
//   1. register_type() 必须在 create_topic() 之前 —— create_topic 会去
//      查找已注册的类型，找不到就返回 nullptr（源码里是硬性检查）。
//   2. create_publisher() 和 create_topic() 之间没有先后要求，两者独立。
//   3. 想改 QoS 的话顺序必须是「取默认值 → 改字段 → 用它创建」，
//      因为 get_default_*_qos() 是【整体赋值】而不是合并字段。
//
// 编译后的程序在 build/cpp/01_basic_api/basic_publisher。
// 先启动任意一个订阅端，再运行本程序。

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// Fast DDS 的头文件按实体分目录存放，用到哪个就 include 哪个。
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

// fastddsgen 从 Basic.idl 生成的两个头文件：
#include "BasicTypes.hpp"              // 纯数据结构 HelloWorld
#include "BasicTypesPubSubTypes.hpp"   // DDS 类型适配类 HelloWorldPubSubType

using namespace eprosima::fastdds::dds;

int main()
{
    // ── 1. 工厂 ────────────────────────────────────────────────────────────
    // DomainParticipantFactory 是进程级单例，用 get_instance() 取。
    DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();

    // ── 2. 参与者 ──────────────────────────────────────────────────────────
    // create_participant(域号, QoS)。域号是个整数，只有域号相同的进程
    // 才会互相发现；不同章节的实验想互不干扰，就换一个域号。
    // PARTICIPANT_QOS_DEFAULT 是"用默认值"的哨兵常量，不需要先取默认 QoS。
    DomainParticipant* participant = factory->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    if (nullptr == participant)
    {
        std::cerr << "创建 DomainParticipant 失败\n";
        return 1;
    }

    // ── 3. 注册数据类型 ────────────────────────────────────────────────────
    // 把 IDL 生成的结构体和 DDS 中间件关联起来。register_type() 内部还会
    // 顺带注册类型的自描述信息（TypeObject），供发现阶段使用。
    // 这一步【必须】在 create_topic 之前。
    TypeSupport type(new HelloWorldPubSubType());
    if (RETCODE_OK != type.register_type(participant))
    {
        std::cerr << "注册类型失败\n";
        return 1;
    }

    // ── 4. Topic ──────────────────────────────────────────────────────────
    // 三个参数：话题名、类型名、QoS。
    // 话题名和类型名【两端必须完全一致】，否则发现阶段匹配不上。
    // type.get_type_name() 来自生成代码，这里的值是 "HelloWorld"。
    Topic* topic = participant->create_topic(
        "HelloWorldTopic", type.get_type_name(), TOPIC_QOS_DEFAULT);
    if (nullptr == topic)
    {
        std::cerr << "创建 Topic 失败\n";
        return 1;
    }

    // ── 5. Publisher 和 DataWriter ────────────────────────────────────────
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (nullptr == publisher)
    {
        std::cerr << "创建 Publisher 失败\n";
        return 1;
    }

    // 这里演示一下怎么改 QoS —— 顺序很重要：
    //   ① 先拿一份默认值 ② 改其中的字段 ③ 再拿这份 QoS 去创建实体
    // 如果把 ② 写在 ① 前面，你设的值会被随后的 get_default_datawriter_qos()
    // 整体覆盖掉（那个函数是赋值，不是合并）。官方的 hello_world 示例
    // 这里就写反了，是个值得留意的坑。
    DataWriterQos writer_qos = DATAWRITER_QOS_DEFAULT;
    publisher->get_default_datawriter_qos(writer_qos);
    writer_qos.history().depth = 10;                          // 每个实例本地缓存 10 条历史
    writer_qos.reliability().kind = RELIABLE_RELIABILITY_QOS; // 可靠传输：重传直到对端确认

    DataWriter* writer = publisher->create_datawriter(topic, writer_qos);
    if (nullptr == writer)
    {
        std::cerr << "创建 DataWriter 失败\n";
        return 1;
    }

    // ── 6. 等发现完成 ──────────────────────────────────────────────────────
    // DDS 是去中心化的：writer 和 reader 靠后台的发现协议互相找到对方，
    // 这需要一点时间。匹配状态里的 current_count 就是"当前有几个对端"。
    // 不等就直接写，样本可能在还没有 reader 的情况下被丢掉。
    PublicationMatchedStatus matched;
    std::cout << "等待订阅端...\n";
    while (0 == matched.current_count)
    {
        writer->get_publication_matched_status(matched);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "已匹配 " << matched.current_count << " 个订阅端\n";

    // ── 7. 发送 ────────────────────────────────────────────────────────────
    HelloWorld sample;
    for (unsigned long i = 0; i < 10; ++i)
    {
        sample.index(i);
        sample.message("hello from cpp publisher " + std::to_string(i));

        // write() 把样本交给中间件。返回 RETCODE_OK 只表示中间件收下了，
        // 不代表对端已经收到 —— DDS 是异步的，真要确认得用
        // writer->wait_for_acknowledgments()。
        if (RETCODE_OK != writer->write(&sample))
        {
            std::cerr << "发送 index=" << i << " 失败\n";
        }
        std::cout << "发送 index=" << i << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ── 8. 清理 ────────────────────────────────────────────────────────────
    // delete_contained_entities() 会连带删掉这个参与者下面的
    // Publisher / Subscriber / Topic / DataWriter / DataReader，
    // 所以不用（也不该）逐个 delete 那些指针。
    participant->delete_contained_entities();
    factory->delete_participant(participant);

    std::cout << "发布端退出\n";
    return 0;
}
