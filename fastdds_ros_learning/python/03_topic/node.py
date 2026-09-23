#!/usr/bin/env python3
"""第 3 章 · 把 DDS Topic 包成 ROS 2 风格的发布订阅

── ROS 2 里的写法 ──────────────────────────────────────────────────────────
    node = rclpy.create_node("chatter")
    pub  = node.create_publisher(Chatter, "topic", 10)
    sub  = node.create_subscription(Chatter, "topic", callback, 10)
    rclpy.spin(node)

── 本章做的事 ──────────────────────────────────────────────────────────────
    把 DDS 实体包成同样形状的类：
      Node          持有一个 DomainParticipant
      Publisher     包 DataWriter，对外只有 publish()
      Subscription  包 DataReader，回调在 on_data_available() 里触发

    这个映射是【一对一】的 —— 话题功能在 DDS 里本来就是原生的，
    不需要像第 2/4/5 章那样"用几条 Topic 拼出一个协议"：

      create_publisher(Chatter, topic, 10)     →  register_type
                                                  + create_topic
                                                  + create_publisher
                                                  + create_datawriter
      create_subscription(Chatter, topic, cb, 10) → register_type
                                                  + create_topic
                                                  + create_subscriber
                                                  + create_datareader
      pub.publish(msg)                         →  DataWriter.write()
      rclpy.spin(node)                         →  （没有对应物，见下）

    Python 一行，DDS 四步 —— 封装的意义就是把那四步收进一个构造函数里。

── 匹配规则：三个条件全满足才连得上 ────────────────────────────────────────

               发布端                        订阅端
       ① Topic 名   "/demo/chatter"   ==?   "/demo/chatter"     ✓
       ② 类型名     "Chatter"         ==?   "Chatter"           ✓
       ③ QoS 兼容   RELIABLE         兼容?  RELIABLE            ✓
                          │                     │
                          └──────────┬──────────┘
                                     ▼
                              匹配成功，建立连接

    任意一条不满足 → 静默不匹配。不报错、不警告，两边各自跑到超时。
    排查匹配问题要看 `fastdds discovery -i 0 -l`，别等报错。

── 三个必须知道的差异 ──────────────────────────────────────────────────────
    1. 【spin() 的含义不同】—— 这是和 ROS 最本质的差异：

       ROS 2（rclpy）                       本章（Fast DDS）
       ───────────────                      ────────────────
       中间件线程 → 放进【回调队列】          中间件线程 → 直接调用
                                                 on_data_available()
                                                    └─ 执行你的 callback
       主线程 spin() → 从队列取 → 跑 callback
                                            主线程 spin() → 只是 while+sleep
                                               ↑ 把主线程卡住，防止程序退出
                                                 、节点被析构

       ★ ROS 那边不 spin 回调永远不跑；
         DDS 这边不 spin 回调照样跑，spin 只是个别退出的等待。

       由此推出两条实践约束：
         - 回调里不能长时间阻塞，否则会拖住中间件的收发线程；
         - 回调里的共享数据要自己加锁（ROS 由 executor 串行化，这边没有）。

    2. ROS 2 有 namespace / remapping / 命名规则那一套；这里 Topic 名就是
       DDS 上真正的名字，不经过任何改名。
    3. QoS 直接暴露 DDS 的。ROS 2 的 QoS 是它自己的一套语义，最终由 RMW
       映射到 DDS QoS；这里不做那层映射。

── 【Python 特有】每个样本都要新建对象 ──────────────────────────────────────

    复用同一个对象（第 1 章的写法）         每轮新建（本文件的写法）
    ─────────────────────────────         ──────────────────────
    sample = Chatter()                    while True:
    while take_next_sample(sample, info):     sample = Chatter()
        callback(sample)  ← 传引用             if take_next_sample(...) != OK: break
              │                               callback(sample)  ← 传引用
              ▼                                     │
    回调如果存了引用，                       回调存了引用也没问题，
    循环几轮后所有引用                        每个引用指向各自的对象
    都指向【最后一条】数据 ⚠

    第 1 章在循环里直接 print，复用没问题；本章要把样本交给用户回调，
    回调可能把引用存起来，所以必须每次新建。

对应关系：本文件与 cpp/03_topic/node.hpp 逐行对应，可以对照着读。
"""

import sys
from pathlib import Path

# fastddsgen 生成的 Python 类型模块（TopicTypes.py）由 CMake 放在构建目录下。
# 这不是共享封装，只是「生成代码的存放位置」；类型定义见同目录 TopicTypes.idl。
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "03_topic"))

import datetime
import time

import fastdds
import TopicTypes   # 由 TopicTypes.idl 生成：TopicTypes.Chatter / TopicTypes.ChatterPubSubType


def to_duration(timeout):
    """把 datetime.timedelta 转成 fastdds.Duration_t。

    Duration_t 内部是（int32 秒, uint32 纳秒），Python 绑定只暴露双参数构造，
    所以必须拆成秒和纳秒两部分传 —— 直接传总纳秒数超过 2^32 会抛 TypeError。
    """
    total = timeout.total_seconds()
    seconds = int(total)
    nanos = int(round((total - seconds) * 1e9))
    return fastdds.Duration_t(seconds, nanos)


# ── 发布者 ─────────────────────────────────────────────────────────────────
class Publisher:
    """对应 rclpy 的 Publisher。对外只有 publish()，把 DataWriter 藏起来。"""

    def __init__(self, writer):
        self._writer = writer

    def publish(self, msg):
        """对应 rclpy 的 publisher.publish(msg)。
        返回后不代表对端已经收到 —— DDS 是异步的。"""
        self._writer.write(msg)

    def wait_for_subscription(self, timeout=datetime.timedelta(seconds=10)):
        """等至少有一个订阅者匹配上。不等就发的话，样本可能在还没有
        订阅者的时候被丢掉。"""
        deadline = datetime.datetime.now() + timeout
        status = fastdds.PublicationMatchedStatus()
        while datetime.datetime.now() < deadline:
            self._writer.get_publication_matched_status(status)
            if status.current_count > 0:
                return True
            time.sleep(0.05)
        return False


# ── 订阅者 ─────────────────────────────────────────────────────────────────
class Subscription(fastdds.DataReaderListener):
    """对应 rclpy 的 Subscription。

    它【同时是】DDS 的 DataReaderListener：create_datareader() 时把 self 传进去，
    数据到了 Fast DDS 就在自己的内部线程上调 on_data_available()，
    我们把样本取出来交给用户回调。

    回调运行在中间件线程上，所以：
      * 不要在回调里做长时间阻塞的操作，会拖住 DDS 的收发线程；
      * 回调里碰到的共享数据要自己加锁。
    """

    def __init__(self, callback):
        super().__init__()
        self._callback = callback
        self._reader = None

    def on_data_available(self, reader):
        info = fastdds.SampleInfo()
        # 必须 while 排空：回调只保证「此刻有数据」，不保证只有一条。
        while True:
            # 每轮都新建一个样本对象，而不是复用同一个 ——
            # 因为下面要把它交给用户回调，回调可能会把引用存起来。
            # 复用的话那些引用最终都会指向同一条数据。
            sample = TopicTypes.Chatter()
            if reader.take_next_sample(sample, info) != fastdds.RETCODE_OK:
                break
            if not info.valid_data:   # dispose / unregister 等生命周期事件，不是数据
                continue
            self._callback(sample)

    def wait_for_publisher(self, timeout=datetime.timedelta(seconds=10)):
        if self._reader is None:
            return False
        deadline = datetime.datetime.now() + timeout
        status = fastdds.SubscriptionMatchedStatus()
        while datetime.datetime.now() < deadline:
            self._reader.get_subscription_matched_status(status)
            if status.current_count > 0:
                return True
            time.sleep(0.05)
        return False


# ── 节点 ───────────────────────────────────────────────────────────────────
class Node:
    """对应 rclpy 的 Node。持有一个 DomainParticipant，
    以及这个参与者下面的 Topic / Publisher / Subscriber 实体。"""

    def __init__(self, name, domain_id=0):
        self.name = name

        # ① 工厂 → 参与者。域号相同的进程才能互相发现，本章都用域 0。
        factory = fastdds.DomainParticipantFactory.get_instance()
        participant_qos = fastdds.DomainParticipantQos()
        factory.get_default_participant_qos(participant_qos)
        self._participant = factory.create_participant(domain_id, participant_qos)

        # ② 注册类型。一个节点里同一种类型注册一次就够，重复注册同名类型会
        #    返回 RETCODE_PRECONDITION_NOT_MET。必须在 create_topic 之前。
        self._type = TopicTypes.ChatterPubSubType()
        self._participant.register_type(fastdds.TypeSupport(self._type))

        # ③ Publisher / Subscriber 是容器，一个节点共用一个就够，用到再建。
        self._publisher = None
        self._subscriber = None

        # ④ 节点持有所有创建过的端点，保证它们的生命周期不短于节点。
        #    尤其是 Subscription —— 它作为 listener 被 DataReader 持有引用，
        #    提前被垃圾回收会导致 DDS 线程回调到已释放的对象。
        self._publishers = []
        self._subscriptions = []

        self._running = True

    def create_publisher(self, topic_name, history_depth=10):
        """对应 rclpy 的 node.create_publisher(Chatter, topic, qos_depth)。"""
        # 建 Topic：名字 + 类型。必须在 register_type 之后。
        topic_qos = fastdds.TopicQos()
        self._participant.get_default_topic_qos(topic_qos)
        topic = self._participant.create_topic(
            topic_name, self._type.get_name(), topic_qos)

        if self._publisher is None:
            publisher_qos = fastdds.PublisherQos()
            self._participant.get_default_publisher_qos(publisher_qos)
            self._publisher = self._participant.create_publisher(publisher_qos)

        # 想改 QoS 的顺序是「取默认值 → 改字段 → 再创建」：
        # get_default_datawriter_qos() 是【整体赋值】而不是合并字段，
        # 顺序写反的话你设的值会被它覆盖掉。
        writer_qos = fastdds.DataWriterQos()
        self._publisher.get_default_datawriter_qos(writer_qos)
        writer_qos.history().depth = history_depth

        writer = self._publisher.create_datawriter(topic, writer_qos)

        pub = Publisher(writer)
        self._publishers.append(pub)
        return pub

    def create_subscription(self, topic_name, callback, history_depth=10):
        """对应 rclpy 的 node.create_subscription(Chatter, topic, callback, qos_depth)。"""
        topic_qos = fastdds.TopicQos()
        self._participant.get_default_topic_qos(topic_qos)
        topic = self._participant.create_topic(
            topic_name, self._type.get_name(), topic_qos)

        if self._subscriber is None:
            subscriber_qos = fastdds.SubscriberQos()
            self._participant.get_default_subscriber_qos(subscriber_qos)
            self._subscriber = self._participant.create_subscriber(subscriber_qos)

        sub = Subscription(callback)

        reader_qos = fastdds.DataReaderQos()
        self._subscriber.get_default_datareader_qos(reader_qos)
        reader_qos.history().depth = history_depth

        # 建 DataReader，并把 sub 自己作为监听器传进去 ——
        # 这是「数据到了回调我」和「主线程主动轮询」的分水岭。
        sub._reader = self._subscriber.create_datareader(topic, reader_qos, sub)

        self._subscriptions.append(sub)
        return sub

    def spin(self):
        """对应 rclpy.spin(node)。

        【注意】这里只是阻塞主线程，回调并不需要它来驱动 ——
        回调跑在 DDS 内部线程上。见文件开头的差异说明。
        """
        while self._running:
            time.sleep(0.1)

    def shutdown(self):
        self._running = False

    def destroy(self):
        """删参与者。delete_contained_entities() 会连带删掉它下面的
        Topic / Publisher / Subscriber / DataWriter / DataReader。"""
        if self._participant is not None:
            self._participant.delete_contained_entities()
            fastdds.DomainParticipantFactory.get_instance().delete_participant(self._participant)
            self._participant = None
