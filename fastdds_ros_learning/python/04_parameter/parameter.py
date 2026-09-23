#!/usr/bin/env python3
"""第 4 章 · 用 DDS 实现 ROS 2 风格的参数系统

── ROS 2 里的参数长什么样 ──────────────────────────────────────────────────
    node.declare_parameter("speed", 1)      # 声明，带默认值
    node.get_parameter("speed").value       # 读
    node.set_parameters([Parameter("speed", 5)])   # 写
    node.add_on_set_parameters_callback(cb) # 参数被改时收到通知

── DDS 里怎么拼出来 ────────────────────────────────────────────────────────
    参数系统是"请求-响应"（和第 2 章的服务同构），外加一条单向的通知通道：

      客户端 --ParamRequest-->  /demo/parameters/request  --> 服务端
      客户端 <--ParamResponse-- /demo/parameters/response <-- 服务端
      所有人 <--ParamEvent----- /demo/parameter_events    <-- 服务端

    第三条 Topic 就是 ROS 2 里的 parameter_events：它不是谁请求的响应，
    而是"参数变了"的广播。任何进程订阅它都能感知到参数变化。

── 和第 2 章的关系：服务骨架 + 一条广播通道 ────────────────────────────────

    ┌─ 第 2 章·服务 ─────────────────┐   ┌─ 第 4 章·参数 ────────────────┐
    │  请求 Topic  ──request_id──┐   │   │  请求 Topic  ──request_id──┐ │
    │  响应 Topic  ──request_id──┘   │   │  响应 Topic  ──request_id──┘ │
    │        ↑ 靠 id 配对            │   │        ↑ 靠 id 配对          │
    │                                │   │  参数事件 Topic  ← 新增！     │
    │                                │   │    纯广播，没有配对           │
    └────────────────────────────────┘   └───────────────────────────────┘

    Service/Client 里那一套 Endpoint + request_id 配对，参数系统原样照搬；
    多出来的只有一条单向的 ParamEvent writer。

── 为什么事件要单独占一条 Topic ────────────────────────────────────────────

    ✗ 塞进响应里：响应只回给【发起修改的那个客户端】

        客户端 A ──set──> 服务端 ──response──> 客户端 A   ← 只有 A 知道
                                            （B、C 什么都没收到）

    ✓ 单独开一条广播 Topic：服务端只写一次，DDS 复制给所有订阅者

        客户端 A ──set──> 服务端
                            ├──response──> 客户端 A       ← 谁问的给谁
                            │
                            └──ParamEvent─> /demo/parameter_events
                                              ├──> 监听进程 monitor
                                              ├──> 客户端 B（如果订了）
                                              └──> 客户端 C（如果订了）

    参数变化是【需要广播出去的全局事件】，不是某个请求的私有响应。
    两者语义不同，在 DDS 上必须占两条不同的 Topic。这就是 ROS 2 里
    parameter_events 话题存在的理由。

── 参数表在哪里 ────────────────────────────────────────────────────────────

    ┌─ 服务端进程 ─────────────────────────────────────────┐
    │   self._parameters = {}      ← 就是它，一个普通 dict   │
    │        {"speed": 5}                                 │
    │   和 DDS 没有任何关系                                 │
    └─────────────────────────────────────────────────────┘
                             ▲
           DDS 提供的只是「传输」这一层：
                             │
    ┌────────────────────────┴────────────────────────────┐
    │  序列化 → RTPS → 网络 → 反序列化 → 放进对端缓存        │
    └─────────────────────────────────────────────────────┘

    参数的【语义】全在你自己的代码里 —— 什么算"已声明"、读未声明的
    参数怎么办、改了要不要通知，都是你定义的。

── 本文件的结构 ────────────────────────────────────────────────────────────
    1. Endpoint         —— 最小的话题端点，全章唯一直接碰 DDS 的地方
    2. ParameterServer  —— 服务端：声明/读取/修改 + 发事件
    3. ParameterClient  —— 客户端：declare / get / set

对应关系：本文件与 cpp/04_parameter/parameter.hpp 逐行对应，可以对照着读。
"""

import sys
from pathlib import Path

# fastddsgen 生成的 Python 类型模块（ParameterTypes.py）由 CMake 放在构建目录下。
# 这不是共享封装，只是「生成代码的存放位置」。
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "04_parameter"))

import datetime
import time

import fastdds
import ParameterTypes   # 生成代码：ParameterTypes.ParamRequest / ParamResponse / ParamEvent


def to_duration(timeout):
    """把 datetime.timedelta 转成 fastdds.Duration_t。

    Duration_t 内部是（int32 秒, uint32 纳秒），Python 绑定只暴露双参数构造，
    所以必须拆成秒和纳秒两部分传 —— 直接传总纳秒数超过 2^32 会抛 TypeError。
    """
    total = timeout.total_seconds()
    seconds = int(total)
    nanos = int(round((total - seconds) * 1e9))
    return fastdds.Duration_t(seconds, nanos)


def create_participant(domain_id=0):
    factory = fastdds.DomainParticipantFactory.get_instance()
    participant_qos = fastdds.DomainParticipantQos()
    factory.get_default_participant_qos(participant_qos)
    return factory.create_participant(domain_id, participant_qos)


def delete_participant(participant):
    if participant is None:
        return
    participant.delete_contained_entities()
    fastdds.DomainParticipantFactory.get_instance().delete_participant(participant)


# ── 端点：一个话题上的「发」或「收」 ───────────────────────────────────────
class Endpoint:
    """一个最小的话题端点。reading=True 建 DataReader（收），False 建 DataWriter（发）。

    把所有创建的实体都存成实例属性 —— Python 会回收没人引用的 SWIG 对象，
    被回收的实体可能已经注册进 DDS，导致出现很难查的「对象还在但收不到数据」。
    """

    def __init__(self, participant, topic_name, pubsub_type_cls, sample_cls, reading):
        self._sample_cls = sample_cls
        self._reading = reading

        # ① 注册类型。必须在 create_topic 之前 —— create_topic 会去查找
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
            #    第三个参数传 None = 不用回调，改成主动轮询（见 take()）。
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
        self._writer.write(sample)

    def take(self, timeout):
        """阻塞等待最多 timeout，取出一条数据。返回 None 表示超时或只有生命周期事件。"""
        # 注意：wait_for_unread_message 返回 bool（有数据 True / 超时 False），
        # 不是 ReturnCode_t，别拿它和 RETCODE_OK 比较（0 != True 恒成立，
        # 会导致「有数据时反而跳过」）。
        if not self._reader.wait_for_unread_message(to_duration(timeout)):
            return None

        info = fastdds.SampleInfo()
        sample = self._sample_cls()
        if self._reader.take_next_sample(sample, info) != fastdds.RETCODE_OK:
            return None
        if not info.valid_data:
            return None
        return sample

    def wait_for_match(self, timeout=datetime.timedelta(seconds=10)):
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


# ── 参数服务端 ─────────────────────────────────────────────────────────────
class ParameterServer:
    """对应 rclpy 里带参数的那个节点。

    参数表就是一个普通的 dict —— 参数系统的"DDS 部分"只是传输，
    存储和语义全在应用层。
    """

    def __init__(self,
                 request_topic="/demo/parameters/request",
                 response_topic="/demo/parameters/response",
                 events_topic="/demo/parameter_events"):
        self._participant = create_participant()
        self._requests = Endpoint(self._participant, request_topic,
                                  ParameterTypes.ParamRequestPubSubType,
                                  ParameterTypes.ParamRequest, reading=True)
        self._responses = Endpoint(self._participant, response_topic,
                                   ParameterTypes.ParamResponsePubSubType,
                                   ParameterTypes.ParamResponse, reading=False)
        self._events = Endpoint(self._participant, events_topic,
                                ParameterTypes.ParamEventPubSubType,
                                ParameterTypes.ParamEvent, reading=False)
        self._parameters = {}
        self._running = True

    def wait_for_clients(self, timeout=datetime.timedelta(seconds=10)):
        return self._requests.wait_for_match(timeout)

    def spin(self):
        """阻塞循环：取请求 → 处理 → 回响应（修改操作额外补一条事件）。"""
        print("参数服务端就绪，等待请求...", flush=True)
        while self._running:
            request = self._requests.take(datetime.timedelta(milliseconds=200))
            if request is None:
                continue

            response = ParameterTypes.ParamResponse()
            response.request_id(request.request_id())
            response.name(request.name())
            response.success(True)

            op = request.op()
            name = request.name()

            if op == ParameterTypes.ParamOp_PARAM_DECLARE:
                # setdefault 的语义：已经声明过就保留原值。
                self._parameters.setdefault(name, request.value())
                response.value(self._parameters[name])
                print(f"声明参数 {name} = {response.value()}", flush=True)

            elif op == ParameterTypes.ParamOp_PARAM_GET:
                if name not in self._parameters:
                    # 读一个没声明过的参数：如实返回失败，而不是悄悄给个 0
                    # —— 后者会让调用方以为参数存在。
                    response.success(False)
                    response.value(0)
                    print(f"读取失败：参数 {name} 未声明", flush=True)
                else:
                    response.value(self._parameters[name])
                    print(f"读取参数 {name} = {response.value()}", flush=True)

            elif op == ParameterTypes.ParamOp_PARAM_SET:
                if name not in self._parameters:
                    response.success(False)
                    response.value(0)
                    print(f"修改失败：参数 {name} 未声明", flush=True)
                else:
                    self._parameters[name] = request.value()
                    response.value(self._parameters[name])
                    print(f"修改参数 {name} = {response.value()}", flush=True)

                    # 【和服务的区别】改参数额外广播一条事件。
                    # 这条不是给请求方的响应，而是给所有订阅者的通知。
                    event = ParameterTypes.ParamEvent()
                    event.name(name)
                    event.value(self._parameters[name])
                    self._events.publish(event)

            self._responses.publish(response)

    def stop(self):
        self._running = False

    def destroy(self):
        delete_participant(self._participant)


# ── 参数客户端 ─────────────────────────────────────────────────────────────
class ParameterClient:
    """对应 rclpy 的 node.declare_parameter / get_parameter / set_parameters。"""

    def __init__(self,
                 request_topic="/demo/parameters/request",
                 response_topic="/demo/parameters/response"):
        self._participant = create_participant()
        self._requests = Endpoint(self._participant, request_topic,
                                  ParameterTypes.ParamRequestPubSubType,
                                  ParameterTypes.ParamRequest, reading=False)
        self._responses = Endpoint(self._participant, response_topic,
                                   ParameterTypes.ParamResponsePubSubType,
                                   ParameterTypes.ParamResponse, reading=True)
        self._next_request_id = 1

    def wait_for_server(self, timeout=datetime.timedelta(seconds=10)):
        return self._requests.wait_for_match(timeout) and self._responses.wait_for_match(timeout)

    def declare_parameter(self, name, default_value, timeout=datetime.timedelta(seconds=5)):
        """声明参数。返回 (success, value)。"""
        return self._request(ParameterTypes.ParamOp_PARAM_DECLARE, name, default_value, timeout)

    def get_parameter(self, name, timeout=datetime.timedelta(seconds=5)):
        """读取参数。返回 (success, value)。"""
        return self._request(ParameterTypes.ParamOp_PARAM_GET, name, 0, timeout)

    def set_parameter(self, name, value, timeout=datetime.timedelta(seconds=5)):
        """修改参数。返回 (success, value)。"""
        return self._request(ParameterTypes.ParamOp_PARAM_SET, name, value, timeout)

    def _request(self, op, name, value, timeout):
        request = ParameterTypes.ParamRequest()
        request.request_id(self._next_request_id)
        self._next_request_id += 1
        request.op(op)
        request.name(name)
        request.value(value)
        self._requests.publish(request)

        deadline = datetime.datetime.now() + timeout
        while datetime.datetime.now() < deadline:
            left = deadline - datetime.datetime.now()
            response = self._responses.take(max(left, datetime.timedelta(0)))
            if response is None:
                continue
            if response.request_id() == request.request_id():
                return response.success(), response.value()
            # id 不匹配：不是给我的响应，丢弃。
        return False, 0

    def destroy(self):
        delete_participant(self._participant)
