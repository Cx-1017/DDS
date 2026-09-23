#!/usr/bin/env python3
"""第 1 章 · Fast DDS 裸 API —— 发布端

这个文件不用任何自定义封装，完整走一遍 Fast DDS 实体的创建流程。

Python 绑定是 C++ API 的近乎 1:1 映射，所以下面每一步都能在
cpp/01_basic_api/basic_publisher.cpp 里找到对应写法：

    DomainParticipantFactory        进程内单例，一切的入口
      └── DomainParticipant         参与者：本进程在某个 DDS 域里的身份
            ├── TypeSupport         类型：怎么把 HelloWorld 变成字节
            └── Topic               话题：把一个【名字】和一个【类型】绑在一起
                  └── Publisher          发布侧容器（订阅侧对应 Subscriber）
                        └── DataWriter   真正执行发送的实体

Python 绑定和 C++ 用法上的三点差异，注意别踩：
  1. Python 里每个实体都由它的【父实体】创建，而不是各自 new。
  2. QoS 不能像 C++ 那样直接写 *_QOS_DEFAULT 哨兵，要先造一个空的
     Qos 对象、用 get_default_xxx_qos() 填充它，再传给 create_xxx()。
  3. 想改 QoS 的顺序同样是「取默认值 → 改字段 → 用它创建」，因为
     get_default_xxx_qos() 是整体赋值不是合并字段。

注意生成的 Python 模块叫 BasicTypes（跟着 BasicTypes.idl 的文件名走），
而里面的类型叫 HelloWorld（跟着 IDL 里的 struct 名走），两者是独立的。

运行前先确认 CMake 已经构建过（生成 BasicTypes.py 到 build/python/01_basic_api/）。
先启动任意一个订阅端，再运行本程序。
"""

import sys
import time
from pathlib import Path

# fastddsgen 生成的 Python 类型模块（BasicTypes.py）由 CMake 放在构建目录下。
# 这不是共享封装，只是「生成代码的存放位置」；本章的类型定义见同目录的 BasicTypes.idl。
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "01_basic_api"))

import fastdds       # Fast DDS 的 Python 绑定本体
import BasicTypes    # 由 BasicTypes.idl 生成：BasicTypes.HelloWorld / BasicTypes.HelloWorldPubSubType

# 话题名和类型名必须和订阅端完全一致，否则发现阶段匹配不上。
TOPIC_NAME = "HelloWorldTopic"
TYPE_NAME = "HelloWorld"


def main():
    # ── 1. 工厂 ────────────────────────────────────────────────────────────
    # DomainParticipantFactory 是进程级单例，用 get_instance() 取。
    factory = fastdds.DomainParticipantFactory.get_instance()

    # ── 2. 参与者 ──────────────────────────────────────────────────────────
    # create_participant(域号, QoS)。域号是个整数，只有域号相同的进程
    # 才会互相发现；不同章节的实验想互不干扰，就换一个域号。
    participant_qos = fastdds.DomainParticipantQos()
    factory.get_default_participant_qos(participant_qos)
    participant = factory.create_participant(0, participant_qos)
    if participant is None:
        print("创建 DomainParticipant 失败", file=sys.stderr)
        return 1

    # ── 3. 注册数据类型 ────────────────────────────────────────────────────
    # 先造类型适配对象，用 TypeSupport 包一层，再交给参与者注册。
    # set_name() 设定的是【DDS 层面的类型名】，两端必须一致。
    # 这一步【必须】在 create_topic 之前 —— create_topic 会去查找
    # 已注册的类型，找不到就返回 None。
    topic_type = BasicTypes.HelloWorldPubSubType()
    topic_type.set_name(TYPE_NAME)
    participant.register_type(fastdds.TypeSupport(topic_type))

    # ── 4. Topic ──────────────────────────────────────────────────────────
    # 三个参数：话题名、类型名、QoS。话题名 + 类型名共同决定能否匹配。
    topic_qos = fastdds.TopicQos()
    participant.get_default_topic_qos(topic_qos)
    topic = participant.create_topic(TOPIC_NAME, topic_type.get_name(), topic_qos)
    if topic is None:
        print("创建 Topic 失败", file=sys.stderr)
        return 1

    # ── 5. Publisher 和 DataWriter ────────────────────────────────────────
    publisher_qos = fastdds.PublisherQos()
    participant.get_default_publisher_qos(publisher_qos)
    publisher = participant.create_publisher(publisher_qos)

    # 改 QoS 的正确顺序：① 取默认值 ② 改字段 ③ 用它创建。
    # 顺序写反的话，你设的值会被随后的 get_default_datawriter_qos() 覆盖掉
    # （那是整体赋值，不是合并）。官方的 HelloWorldExample.py 这里也是对的，
    # 但 C++ 版示例写反了，可以对照着看。
    writer_qos = fastdds.DataWriterQos()
    publisher.get_default_datawriter_qos(writer_qos)
    writer_qos.history().depth = 10
    writer_qos.reliability().kind = fastdds.RELIABLE_RELIABILITY_QOS
    writer = publisher.create_datawriter(topic, writer_qos)
    if writer is None:
        print("创建 DataWriter 失败", file=sys.stderr)
        return 1

    # ── 6. 等发现完成 ──────────────────────────────────────────────────────
    # DDS 是去中心化的：writer 和 reader 靠后台的发现协议互相找到对方，
    # 这需要一点时间。current_count 就是「当前有几个对端」。
    # 不等就直接写，样本可能在还没有 reader 的情况下被丢掉。
    matched = fastdds.PublicationMatchedStatus()
    print("等待订阅端...")
    while True:
        writer.get_publication_matched_status(matched)
        if matched.current_count:
            break
        time.sleep(0.1)
    print(f"已匹配 {matched.current_count} 个订阅端")

    # ── 7. 发送 ────────────────────────────────────────────────────────────
    sample = BasicTypes.HelloWorld()
    for i in range(10):
        sample.index(i)
        sample.message(f"hello from python publisher {i}")

        # write() 把样本交给中间件。它返回后不代表对端已经收到 ——
        # DDS 是异步的，真要确认得用 writer.wait_for_acknowledgments()。
        writer.write(sample)
        print("发送 index=", i)
        time.sleep(1)

    # ── 8. 清理 ────────────────────────────────────────────────────────────
    # delete_contained_entities() 会连带删掉这个参与者下面的
    # Publisher / Subscriber / Topic / DataWriter / DataReader，
    # 所以不用（也不该）逐个删除那些对象。
    participant.delete_contained_entities()
    factory.delete_participant(participant)

    print("发布端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
