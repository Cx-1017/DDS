#!/usr/bin/env python3
"""第 1 章 · Fast DDS 裸 API —— 订阅端之二：主线程轮询

和 subscriber_listener.py 只差两点：
  1. create_datareader() 的监听器参数传 None
  2. 取数据变成主线程里的显式循环：等 → 取 → 处理

对初学者来说这个顺序最好懂，因为控制流全在主线程里，不需要理解
「回调在哪个线程上跑」。代价是要自己管循环和超时。

三个和取数相关的 API，用哪个取决于你想不想保留缓存里的数据：
    wait_for_unread_message(超时)  阻塞等待，直到有未读数据（这里用它）
                                   返回 bool：有数据 True / 超时 False
    take_next_sample(sample, info) 取一条，并从缓存里删掉
    read_next_sample(sample, info) 取一条，仍留在缓存里

data_available() 只能被动查「现在有没有」，用它做忙轮询会白白烧 CPU，
阻塞式的 wait_for_unread_message() 才是正解。

运行前先确认 CMake 已经构建过，且 fastdds 绑定在 PYTHONPATH 上。
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


class MatchListener(fastdds.DataReaderListener):
    """轮询模式不需要 listener 来取数据，但监听器还有别的用处：
    这里只用它感知「发布端来了/走了」。也可以完全不要监听器。"""

    def on_subscription_matched(self, reader, info):
        verb = "发现发布端" if info.current_count_change > 0 else "发布端离开"
        print(f"{verb}，当前匹配数={info.current_count}")


def main():
    # ── 1~4 步：工厂 → 参与者 → 注册类型 → Topic（与另两个程序相同）───────
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

    listener = MatchListener()   # 同样要保证不被垃圾回收，见 subscriber_listener.py 注释
    reader_qos = fastdds.DataReaderQos()
    subscriber.get_default_datareader_qos(reader_qos)
    reader = subscriber.create_datareader(topic, reader_qos, listener)

    # ── 6. 主线程轮询 ─────────────────────────────────────────────────────
    print("订阅端已就绪，轮询等待数据（Ctrl-C 退出）...")
    info = fastdds.SampleInfo()
    sample = BasicTypes.HelloWorld()

    try:
        while True:
            # 阻塞等待，直到 reader 上有未读数据。
            # 【注意返回类型】它返回的是 bool 而不是 ReturnCode_t：
            # 有数据 True，超时 False。别习惯性地拿它和 RETCODE_OK 比 ——
            # RETCODE_OK 是 0 而 True 是 1，那样写会导致「有数据时反而跳过」。
            timeout = fastdds.Duration_t(1, 0)    # 1 秒 0 纳秒
            if not reader.wait_for_unread_message(timeout):
                continue    # 超时，回去继续等

            # 等到之后仍然要排空，因为一次可能来了多条。
            while reader.take_next_sample(sample, info) == fastdds.RETCODE_OK:
                if not info.valid_data:   # dispose / unregister 等生命周期事件，不是数据
                    continue
                print(f'收到 index={sample.index()} message="{sample.message()}"')
    except KeyboardInterrupt:
        pass

    # ── 7. 清理 ───────────────────────────────────────────────────────────
    participant.delete_contained_entities()
    factory.delete_participant(participant)
    print("\n订阅端退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
