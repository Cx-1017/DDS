#!/usr/bin/env python3
"""第 1 章 · Fast DDS 裸 API —— 订阅端之一：listener 回调

与 publisher.py 的创建流程完全对称，只是把
    Publisher  → Subscriber
    DataWriter → DataReader
并且 create_datareader() 多传一个 DataReaderListener。

【关键区别：threading】

   Fast DDS 内部线程                          主线程
   ──────────────────────                     ──────────────────
         │                                          │
   收到一个样本                                     │
         │                                          │
         ▼                                          │
   on_data_available(reader)  ← 自动回调            │
         │      ▲                                   │
         │      └─ 这个函数【不是你调的】，是中间件调的
         │                                          │
         ├─ while reader.take_next_sample(...) == OK
         │      排空缓存，处理每一条                 │
         │                                          │
         ▼                                          ▼
    （回调返回，线程继续干别的）         while True: time.sleep(0.2)
                                          ↑ 主线程只是等在这里，
                                            防止程序退出把对象析构掉

三个必须记住的结论：
  1. 取数据的代码不在主线程里，而在中间件线程上被并发调用 ——
     回调里碰到的共享数据要自己加锁。
  2. 回调里不能长时间阻塞，否则会拖住 DDS 的收发线程。
  3. 【Python 特有】listener 对象必须保持引用。reader 只持有它，
     一旦被垃圾回收，回调会**静默失效** —— 不报错，只是再也收不到数据。
     所以下面把它绑在一个活到程序结束的局部变量上。

【和轮询方式的唯一区别】
  listener：create_datareader(topic, qos, listener)   ← 传 listener 对象
  轮询    ：create_datareader(topic, qos, None)       ← 传 None，
            然后自己在主线程里循环 wait_for_unread_message() + take

对照程序：subscriber_polling.py。两种方式没有优劣，看场景选。

运行前先确认 CMake 已经构建过，且 fastdds 绑定在 PYTHONPATH 上
（见仓库根目录 README.md）。
"""

import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "01_basic_api"))

import time

import fastdds
import BasicTypes

TOPIC_NAME = "HelloWorldTopic"
TYPE_NAME = "HelloWorld"


class HelloWorldListener(fastdds.DataReaderListener):
    """reader 上有数据时，Fast DDS 会在自己的内部线程里回调这里。

    继承 DataReaderListener 后按需覆盖关心的事件：
        on_data_available        有数据可取（这里用它）
        on_subscription_matched  发现/失去发布端
        on_sample_lost           中间件丢样本了（缓存满等）
    """

    def on_data_available(self, reader):
        info = fastdds.SampleInfo()
        sample = BasicTypes.HelloWorld()

        # take_next_sample() 取出一条并把它从 reader 的缓存里【删除】。
        # 想取出来但保留在缓存里，用 read_next_sample()。
        #
        # 必须用 while 循环排空：回调只保证「此刻 reader 上有数据」，
        # 并不保证只有一条。一次只取一条的话，积压的数据要等下一次回调
        # 才处理得完，高频率下发会越来越落后。
        # （官方的 HelloWorldExample.py 这里就只取了一条，是个常见错误。）
        while reader.take_next_sample(sample, info) == fastdds.RETCODE_OK:
            # info.valid_data 为 False 表示这条不是真实数据，而是
            # dispose（标记实例已删除）或 unregister 之类的生命周期事件。
            # 不判断的话会把它们当成正常消息打印出来。
            # （官方示例也漏了这个判断。）
            if not info.valid_data:
                continue
            print(f'收到 index={sample.index()} message="{sample.message()}"')

    def on_subscription_matched(self, reader, info):
        # current_count_change > 0 表示新增了一个发布端，< 0 表示有发布端离开。
        verb = "发现发布端" if info.current_count_change > 0 else "发布端离开"
        print(f"{verb}，当前匹配数={info.current_count}")


def main():
    # ── 1~4 步和发布端一样：工厂 → 参与者 → 注册类型 → Topic ──────────────
    factory = fastdds.DomainParticipantFactory.get_instance()

    participant_qos = fastdds.DomainParticipantQos()
    factory.get_default_participant_qos(participant_qos)
    participant = factory.create_participant(0, participant_qos)

    topic_type = BasicTypes.HelloWorldPubSubType()
    topic_type.set_name(TYPE_NAME)
    participant.register_type(fastdds.TypeSupport(topic_type))   # 必须在 create_topic 之前

    topic_qos = fastdds.TopicQos()
    participant.get_default_topic_qos(topic_qos)
    topic = participant.create_topic(TOPIC_NAME, topic_type.get_name(), topic_qos)

    # ── 5. Subscriber 和 DataReader ───────────────────────────────────────
    subscriber_qos = fastdds.SubscriberQos()
    participant.get_default_subscriber_qos(subscriber_qos)
    subscriber = participant.create_subscriber(subscriber_qos)

    # 这个 listener 引用必须一直活着 —— reader 持有它，并在自己的线程里
    # 调用它。如果它被 Python 垃圾回收了，回调就会静默失效（不报错，
    # 只是再也收不到数据）。所以这里把它绑在 main 的局部变量上。
    listener = HelloWorldListener()

    # create_datareader(话题, QoS, 监听器)。
    # 第三个参数传监听器就是「数据到了回调我」；传 None 就变成轮询模式，
    # 需要自己调 wait_for_unread_message() / take_next_sample()
    # —— 见 subscriber_polling.py。
    reader_qos = fastdds.DataReaderQos()
    subscriber.get_default_datareader_qos(reader_qos)
    reader = subscriber.create_datareader(topic, reader_qos, listener)

    # ── 6. 等待 ───────────────────────────────────────────────────────────
    # 数据在 DDS 内部线程里被回调处理，主线程只需要等在这里。
    print("订阅端已就绪，等待数据（Ctrl-C 退出）...")
    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass

    # ── 7. 清理 ───────────────────────────────────────────────────────────
    participant.delete_contained_entities()
    factory.delete_participant(participant)
    print("\n订阅端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
