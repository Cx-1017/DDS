#!/usr/bin/env python3
"""第 2 章 · 用 DDS 实现 ROS 2 风格的服务

── ROS 2 里的服务长什么样 ──────────────────────────────────────────────────
    rclpy:   node.create_service(Add, "add", handler)
             node.create_client(Add, "add")
             client.call_async(request)
    服务是"请求-响应"语义：客户端发一个请求，服务端算完把结果发回来。

── DDS 里怎么拼出来 ────────────────────────────────────────────────────────
    DDS 只有发布订阅，没有 RPC。服务要用两条 Topic 拼：

      客户端 --AddRequest-->  /demo/add/request  --> 服务端
      客户端 <--AddResponse-- /demo/add/response <-- 服务端

    两条 Topic 之间靠 request_id 关联：响应把请求的 id 原样带回来，
    客户端只认 id 对得上的那条响应，这样多个请求并发也不会串味。

── 一次 call() 的时序 ──────────────────────────────────────────────────────

    Client                                    Server
      │                                          │
      │ ① wait_for_service()                     │  ← 服务端要先起。
      │     轮询两条 Topic 的匹配数                │    不等就发，样本会
      │ <───────── 发现完成 ─────────────────────>│    在没匹配上时被丢掉
      │                                          │
      │ ② request.request_id(1)                  │
      │    request.a(20) / request.b(22)         │
      │                                          │
      │ ③ requests.publish(request)              │
      │ ──────── /demo/add/request ────────────> │ ④ requests.take()
      │                                          │
      │                                          │ ⑤ handler(request)
      │                                          │      sum = 42
      │                                          │
      │                                          │ ⑥ response.request_id(
      │                                          │       request.request_id())
      │                                          │    ⚠ 配对的全部就在这里
      │ ⑧ responses.take()                       │
      │    检查 request_id == 1 ？                │ ⑦ responses.publish(...)
      │ ──────── /demo/add/response ───────────< │
      │    对得上 → 返回；对不上 → 丢掉继续等       │
      │                                          │
    第 ⑥ 步是整章的核心：服务端唯一的义务就是"把 id 原样带回"。

── 本文件的结构 ────────────────────────────────────────────────────────────
    1. Endpoint  —— 一个最小的话题端点（约 60 行）。
                    【这是全章唯一直接碰 DDS 的地方】，构造函数里逐行注释了
                    每一步对应哪个 DDS 实体。想学 DDS 就看这里。
    2. Service   —— 服务端，对应 rclpy 的 create_service + spin
    3. Client    —— 客户端，对应 rclpy 的 create_client + call_async

    Service / Client 只是在 Endpoint 之上加了一层 ROS 风格的 API，
    本身不含新的 DDS 知识。

── 三层结构，每层加的东西不多 ──────────────────────────────────────────────

    ┌─ Service / Client ────────────────────────────────────┐
    │  只做协议逻辑：request_id 怎么发、怎么配对、何时放弃       │
    │  （这两个类里【没有一行】DDS 调用）                       │
    └──────────────────────┬────────────────────────────────┘
                           │ 用两个 Endpoint，
                           │ 一个方向收、一个方向发
    ┌──────────────────────▼────────────────────────────────┐
    │  Endpoint                                             │
    │  register_type → create_topic → create_publisher/     │
    │  create_subscriber → create_datawriter/datareader     │
    │  ★ 全章唯一直接碰 DDS 的地方 ★                          │
    └──────────────────────┬────────────────────────────────┘
                           │
    ┌──────────────────────▼────────────────────────────────┐
    │  Fast DDS 中间件（domain participant / RTPS / 发现协议） │
    └───────────────────────────────────────────────────────┘

对应关系：本文件与 cpp/02_service/service.hpp 逐行对应，可以对照着读。
"""

import sys
from pathlib import Path

# fastddsgen 生成的 Python 类型模块（ServiceTypes.py）由 CMake 放在构建目录下。
# 这不是共享封装，只是「生成代码的存放位置」；本章的类型定义见同目录的 ServiceTypes.idl。
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "02_service"))

import datetime
import time

import fastdds
import ServiceTypes   # 由 ServiceTypes.idl 生成：ServiceTypes.AddRequest / ServiceTypes.AddResponse / ...


# ── 时间转换的小工具 ───────────────────────────────────────────────────────
def to_duration(timeout):
    """把 datetime.timedelta 转成 Fast DDS 的 Duration_t。

    Duration_t 内部是（int32 秒, uint32 纳秒）两个字段，Python 绑定只暴露了
    这个双参数构造。所以【不能】把总秒数直接乘 1e9 当纳秒传进去 ——
    超过 2^32 会直接抛 TypeError，比如 5 秒 = 5e9 纳秒就超了。
    必须拆成秒和纳秒两部分。
    """
    total = timeout.total_seconds()
    seconds = int(total)
    nanos = int(round((total - seconds) * 1e9))
    return fastdds.Duration_t(seconds, nanos)


# ── 参与者的创建与销毁 ─────────────────────────────────────────────────────
def create_participant(domain_id=0):
    """每个「节点」都从这一步开始：拿到工厂，建一个参与者。

    域号相同的进程才能互相发现，本章所有程序都用域 0。
    """
    # DomainParticipantFactory 是进程级单例。
    factory = fastdds.DomainParticipantFactory.get_instance()
    # create_participant(域号, QoS)。默认 QoS 要先取出来填进一个空对象。
    participant_qos = fastdds.DomainParticipantQos()
    factory.get_default_participant_qos(participant_qos)
    return factory.create_participant(domain_id, participant_qos)


def delete_participant(participant):
    """删参与者。delete_contained_entities() 会连带删掉它下面的
    Topic / Publisher / Subscriber / DataWriter / DataReader，
    所以不用逐个删那些对象。"""
    if participant is None:
        return
    participant.delete_contained_entities()
    fastdds.DomainParticipantFactory.get_instance().delete_participant(participant)


# ── 端点：一个话题上的「发」或「收」 ───────────────────────────────────────
class Endpoint:
    """一个最小的话题端点。

    reading=True 建 DataReader（收），False 建 DataWriter（发）。

    注意这里把所有创建的实体都存成了实例属性 —— Python 的垃圾回收会回收
    掉没人引用的 SWIG 对象，而被回收的实体可能已经被 DDS 内部注册过，
    导致后面出现很难查的「对象还在但收不到数据」。保持引用是最省心的做法。
    """

    def __init__(self, participant, topic_name, pubsub_type_cls, sample_cls, reading):
        self._participant = participant
        self._sample_cls = sample_cls
        self._reading = reading

        # ① 注册类型：把生成的数据结构和 DDS 中间件关联起来。
        #    类型名由生成代码设好（等于 IDL 里的 struct 名，例如 "AddRequest"），
        #    两端必须一致才能匹配上。
        #    这一步【必须】在 create_topic 之前 —— create_topic 会去查找
        #    已注册的类型，找不到就返回 None。
        self._topic_type = pubsub_type_cls()
        self._type_support = fastdds.TypeSupport(self._topic_type)
        participant.register_type(self._type_support)

        # ② 建 Topic：把一个【名字】和一个【类型】绑在一起。
        topic_qos = fastdds.TopicQos()
        participant.get_default_topic_qos(topic_qos)
        self._topic = participant.create_topic(
            topic_name, self._topic_type.get_name(), topic_qos)

        if reading:
            # ③ 订阅侧：Subscriber 是容器，DataReader 才是真正收数据的实体。
            #    第三个参数传 None = 不用回调，改成主动轮询（取数见 take()）。
            subscriber_qos = fastdds.SubscriberQos()
            participant.get_default_subscriber_qos(subscriber_qos)
            self._subscriber = participant.create_subscriber(subscriber_qos)

            reader_qos = fastdds.DataReaderQos()
            self._subscriber.get_default_datareader_qos(reader_qos)
            self._reader = self._subscriber.create_datareader(self._topic, reader_qos, None)
            self._writer = None
        else:
            # ③ 发布侧：Publisher 是容器，DataWriter 才是真正发数据的实体。
            publisher_qos = fastdds.PublisherQos()
            participant.get_default_publisher_qos(publisher_qos)
            self._publisher = participant.create_publisher(publisher_qos)

            writer_qos = fastdds.DataWriterQos()
            self._publisher.get_default_datawriter_qos(writer_qos)
            self._writer = self._publisher.create_datawriter(self._topic, writer_qos)
            self._reader = None

    def publish(self, sample):
        """发一条。返回后不代表对端已经收到 —— DDS 是异步的。"""
        self._writer.write(sample)

    def take(self, timeout):
        """阻塞等待最多 timeout（datetime.timedelta），取出一条数据。
        返回 None 表示超时，或只等到了生命周期事件（没有真实数据）。"""
        # 【注意返回类型】wait_for_unread_message() 返回的是 bool 而不是
        # ReturnCode_t：有数据 True / 超时 False。别习惯性地拿它和
        # RETCODE_OK 比较 —— RETCODE_OK 是 0 而 True 是 1，
        # 那样会写成「有数据时反而跳过」。
        if not self._reader.wait_for_unread_message(to_duration(timeout)):
            return None

        info = fastdds.SampleInfo()
        sample = self._sample_cls()
        # take_next_sample() 取一条并从缓存里删掉它。
        if self._reader.take_next_sample(sample, info) != fastdds.RETCODE_OK:
            return None
        # valid_data 为 False 表示这是 dispose / unregister 之类的
        # 生命周期事件，不是真实数据。
        if not info.valid_data:
            return None
        return sample

    def wait_for_match(self, timeout=datetime.timedelta(seconds=10)):
        """等发现完成：等到至少有一个对端匹配上。

        DDS 靠后台的发现协议自动找对端，这需要一点时间；不等就开始发，
        样本可能在还没有对端的情况下被丢掉。
        """
        deadline = datetime.datetime.now() + timeout
        while datetime.datetime.now() < deadline:
            if self._reading:
                status = fastdds.SubscriptionMatchedStatus()
                self._reader.get_subscription_matched_status(status)
            else:
                status = fastdds.PublicationMatchedStatus()
                self._writer.get_publication_matched_status(status)
            if status.current_count > 0:
                return True
            time.sleep(0.05)
        return False


# ── 服务端 ─────────────────────────────────────────────────────────────────
class Service:
    """对应 rclpy 的 node.create_service(...) + spin()。

    内部两个端点：一个读请求，一个写响应。
    """

    def __init__(self, request_topic, response_topic):
        self._participant = create_participant()
        self._requests = Endpoint(self._participant, request_topic,
                                  ServiceTypes.AddRequestPubSubType, ServiceTypes.AddRequest, reading=True)
        self._responses = Endpoint(self._participant, response_topic,
                                   ServiceTypes.AddResponsePubSubType, ServiceTypes.AddResponse, reading=False)
        self._running = True

    def wait_for_clients(self, timeout=datetime.timedelta(seconds=10)):
        """等客户端出现。服务端先跑起来、客户端后跑，是更稳的顺序。"""
        return self._requests.wait_for_match(timeout)

    def spin(self, handler):
        """阻塞循环：取请求 → 调用 handler → 写回响应。对应 rclpy 的 spin()。

        handler 收到 AddRequest，返回 AddResponse（不用自己填 request_id，
        下面会补上）。
        """
        print("服务端就绪，等待请求...", flush=True)
        while self._running:
            request = self._requests.take(datetime.timedelta(milliseconds=200))
            if request is None:
                continue    # 200ms 没请求就回去看看要不要退出，顺便让 Ctrl-C 能生效

            print(f"收到请求 #{request.request_id()}: "
                  f"add({request.a()}, {request.b()})", flush=True)

            response = handler(request)
            # 【关键】把请求的 id 原样带回响应里，客户端靠它配对。
            # ROS 2 的真实服务由 RMW 按 sequence number 自动完成这件事。
            response.request_id(request.request_id())
            self._responses.publish(response)

    def stop(self):
        self._running = False

    def destroy(self):
        delete_participant(self._participant)


# ── 客户端 ─────────────────────────────────────────────────────────────────
class Client:
    """对应 rclpy 的 node.create_client(...) + call_async()。

    同样是两个端点，但方向反过来：写请求、读响应。
    """

    def __init__(self, request_topic, response_topic):
        self._participant = create_participant()
        self._requests = Endpoint(self._participant, request_topic,
                                  ServiceTypes.AddRequestPubSubType, ServiceTypes.AddRequest, reading=False)
        self._responses = Endpoint(self._participant, response_topic,
                                   ServiceTypes.AddResponsePubSubType, ServiceTypes.AddResponse, reading=True)
        self._next_request_id = 1

    def wait_for_service(self, timeout=datetime.timedelta(seconds=10)):
        """等服务端出现。不等就发，请求会在两端还没匹配上时被丢掉，
        客户端只能等到超时 —— 这是 DDS 上做 RPC 最常见的坑。"""
        return self._requests.wait_for_match(timeout) and self._responses.wait_for_match(timeout)

    def call(self, a, b, timeout=datetime.timedelta(seconds=5)):
        """同步调用：发请求 → 等 id 对得上的响应。返回 None 表示超时。

        rclpy 的 call_async() 返回 future，是异步的；这里为了讲清楚配对过程
        写成同步的。
        """
        request = ServiceTypes.AddRequest()
        request.request_id(self._next_request_id)
        self._next_request_id += 1
        request.a(a)
        request.b(b)
        self._requests.publish(request)

        # 只认 id 对得上的响应，对不上的丢掉继续等。
        deadline = datetime.datetime.now() + timeout
        while datetime.datetime.now() < deadline:
            left = deadline - datetime.datetime.now()
            response = self._responses.take(max(left, datetime.timedelta(0)))
            if response is None:
                continue
            if response.request_id() == request.request_id():
                return response
            # id 不匹配：不是给我的响应（可能是别的客户端请求触发的），丢弃。
        return None

    def destroy(self):
        delete_participant(self._participant)
