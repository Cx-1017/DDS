#!/usr/bin/env python3
"""第 5 章 · 用 DDS 实现 ROS 2 风格的 action

── ROS 2 里的 action 长什么样 ──────────────────────────────────────────────
    from rclpy.action import ActionServer, ActionClient
    ActionServer(node, Fibonacci, "fibonacci", execute_callback)
    ActionClient(node, Fibonacci, "fibonacci")
    goal_handle.publish_feedback(fb)
    goal_handle.is_canceling()

    action 是「有状态的长任务」：客户端发一个目标，服务端边跑边报进度，
    跑完给结果，中途还能取消。它比服务多出两件事：
      * 反馈通道（单向、多次）
      * 取消通道（反向、随时）

── DDS 里怎么拼出来 ────────────────────────────────────────────────────────
    四条 Topic，靠 goal_id 关联：

      客户端 --FibonacciGoal------> /demo/fibonacci/goal      --> 服务端
      客户端 <--FibonacciFeedback-- /demo/fibonacci/feedback  <-- 服务端
      客户端 <--FibonacciResult---- /demo/fibonacci/result    <-- 服务端
      客户端 --FibonacciCancel----> /demo/fibonacci/cancel    --> 服务端

    和服务的最大区别：服务一次请求一个响应就结束了，action 的会话是
    【长时间存活】的 —— 服务端要一边算一边发反馈，同时还要盯着取消通道。

── 本文件的结构 ────────────────────────────────────────────────────────────
    1. Endpoint    —— 最小的话题端点，全章唯一直接碰 DDS 的地方
    2. GoalHandle  —— 执行期间交给用户回调的句柄，报反馈 / 查取消都通过它
    3. ActionServer —— 服务端
    4. ActionClient —— 客户端

对应关系：本文件与 cpp/05_action/action.hpp 逐行对应，可以对照着读。
"""

import sys
from pathlib import Path

# fastddsgen 生成的 Python 类型模块（ActionTypes.py）由 CMake 放在构建目录下。
# 这不是共享封装，只是「生成代码的存放位置」。
_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "05_action"))

import datetime
import time

import fastdds
import ActionTypes   # 生成代码：Goal / Feedback / Result / Cancel 四个类型


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
            # ③ 订阅侧。第三个参数传 None = 不用回调，改成主动轮询。
            subscriber_qos = fastdds.SubscriberQos()
            participant.get_default_subscriber_qos(subscriber_qos)
            self._subscriber = participant.create_subscriber(subscriber_qos)

            reader_qos = fastdds.DataReaderQos()
            self._subscriber.get_default_datareader_qos(reader_qos)
            self._reader = self._subscriber.create_datareader(self._topic, reader_qos, None)
            self._writer = None
        else:
            # ③ 发布侧。
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
        # 不是 ReturnCode_t，别拿它和 RETCODE_OK 比较。
        if not self._reader.wait_for_unread_message(to_duration(timeout)):
            return None
        return self._take_now()

    def try_take(self):
        """非阻塞取一条：有就取走，没有立刻返回 None。
        取消通道要用它 —— 检查取消不能把执行流程阻塞住。"""
        return self._take_now()

    def _take_now(self):
        info = fastdds.SampleInfo()
        sample = self._sample_cls()
        # take_next_sample 本身就不阻塞，所以不需要先等。
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


# ── 目标句柄 ───────────────────────────────────────────────────────────────
class GoalHandle:
    """执行期间交给用户回调，对应 rclpy 的 ServerGoalHandle：
        publish_feedback()  ←→ goal_handle.publish_feedback(feedback)
        is_canceling()      ←→ goal_handle.is_canceling()
        goal                ←→ goal_handle.request
    """

    def __init__(self, goal, feedback_endpoint, cancel_endpoint):
        self.goal = goal
        self._feedback = feedback_endpoint
        self._cancel = cancel_endpoint

    def publish_feedback(self, sequence_index, partial_sequence):
        """对应 goal_handle.publish_feedback()。执行途中可以调任意多次。"""
        fb = ActionTypes.FibonacciFeedback()
        fb.goal_id(self.goal.goal_id())
        fb.sequence_index(sequence_index)
        fb.partial_sequence(partial_sequence)
        self._feedback.publish(fb)

    def is_canceling(self):
        """对应 goal_handle.is_canceling()。

        用非阻塞取数把取消通道排空 —— 检查取消不能把执行流程卡住。
        没有取消请求时立刻返回 False，所以可以放在执行循环的每一轮里调用。
        """
        while True:
            cancel = self._cancel.try_take()
            if cancel is None:
                return False
            if cancel.goal_id() == self.goal.goal_id():
                return True
            # 取消的是别的目标：本示例一次只处理一个目标，直接丢弃。


# ── action 服务端 ──────────────────────────────────────────────────────────
class ActionServer:
    """对应 rclpy 的 ActionServer。

    execute 回调收到 GoalHandle，在里面跑长任务：通过 handle 报反馈、
    查是否被取消，最后返回 FibonacciResult。
    """

    def __init__(self, root="/demo/fibonacci"):
        self._participant = create_participant()
        self._goals = Endpoint(self._participant, root + "/goal",
                               ActionTypes.FibonacciGoalPubSubType,
                               ActionTypes.FibonacciGoal, reading=True)
        self._feedbacks = Endpoint(self._participant, root + "/feedback",
                                   ActionTypes.FibonacciFeedbackPubSubType,
                                   ActionTypes.FibonacciFeedback, reading=False)
        self._results = Endpoint(self._participant, root + "/result",
                                 ActionTypes.FibonacciResultPubSubType,
                                 ActionTypes.FibonacciResult, reading=False)
        self._cancels = Endpoint(self._participant, root + "/cancel",
                                 ActionTypes.FibonacciCancelPubSubType,
                                 ActionTypes.FibonacciCancel, reading=True)
        self._running = True

    def wait_for_clients(self, timeout=datetime.timedelta(seconds=10)):
        # 四条通道都要匹配上，缺一条都会导致协议走不通。
        return (self._goals.wait_for_match(timeout)
                and self._feedbacks.wait_for_match(timeout)
                and self._results.wait_for_match(timeout)
                and self._cancels.wait_for_match(timeout))

    def spin(self, execute):
        """阻塞循环：取目标 → 执行 → 发结果。一次处理一个目标。"""
        print("action 服务端就绪，等待目标...", flush=True)
        while self._running:
            goal = self._goals.take(datetime.timedelta(milliseconds=200))
            if goal is None:
                continue

            print(f"收到目标 #{goal.goal_id()}：生成 {goal.order()} 个斐波那契数", flush=True)

            handle = GoalHandle(goal, self._feedbacks, self._cancels)
            result = execute(handle)

            # 目标 id 由服务端保证回填，用户回调不用操心。
            result.goal_id(goal.goal_id())
            self._results.publish(result)

            state = "已取消" if result.canceled() else "已完成"
            print(f"目标 #{goal.goal_id()} {state}，结果 {result.result()}", flush=True)

    def stop(self):
        self._running = False

    def destroy(self):
        delete_participant(self._participant)


# ── action 客户端 ──────────────────────────────────────────────────────────
class ActionClient:
    """对应 rclpy 的 ActionClient。"""

    def __init__(self, root="/demo/fibonacci"):
        self._participant = create_participant()
        self._goals = Endpoint(self._participant, root + "/goal",
                               ActionTypes.FibonacciGoalPubSubType,
                               ActionTypes.FibonacciGoal, reading=False)
        self._feedbacks = Endpoint(self._participant, root + "/feedback",
                                   ActionTypes.FibonacciFeedbackPubSubType,
                                   ActionTypes.FibonacciFeedback, reading=True)
        self._results = Endpoint(self._participant, root + "/result",
                                 ActionTypes.FibonacciResultPubSubType,
                                 ActionTypes.FibonacciResult, reading=True)
        self._cancels = Endpoint(self._participant, root + "/cancel",
                                 ActionTypes.FibonacciCancelPubSubType,
                                 ActionTypes.FibonacciCancel, reading=False)
        self._next_goal_id = 1

    def wait_for_server(self, timeout=datetime.timedelta(seconds=10)):
        return (self._goals.wait_for_match(timeout)
                and self._feedbacks.wait_for_match(timeout)
                and self._results.wait_for_match(timeout)
                and self._cancels.wait_for_match(timeout))

    def send_goal(self, order):
        """对应 rclpy 的 send_goal_async()。返回目标 id。"""
        goal = ActionTypes.FibonacciGoal()
        goal.goal_id(self._next_goal_id)
        self._next_goal_id += 1
        goal.order(order)
        self._goals.publish(goal)
        return goal.goal_id()

    def cancel_goal(self, goal_id):
        """对应 rclpy 的 cancel_goal_async()。"""
        cancel = ActionTypes.FibonacciCancel()
        cancel.goal_id(goal_id)
        self._cancels.publish(cancel)

    def wait_for_result(self, goal_id, on_feedback=None,
                        timeout=datetime.timedelta(seconds=30)):
        """对应 rclpy 的 get_result_async()。返回 FibonacciResult，超时返回 None。

        一边等结果，一边把收到的反馈交给 on_feedback 回调。
        回调在【主线程】里被调用，所以在里面调 cancel_goal() 是安全的。
        """
        deadline = datetime.datetime.now() + timeout
        while datetime.datetime.now() < deadline:
            left = deadline - datetime.datetime.now()

            # 反馈可能来好几条，所以先尽量排空反馈通道。
            while True:
                feedback = self._feedbacks.try_take()
                if feedback is None:
                    break
                if on_feedback is not None and feedback.goal_id() == goal_id:
                    on_feedback(feedback)

            # 再阻塞等一会儿结果。分片等待是为了能及时回头处理反馈。
            wait = min(left, datetime.timedelta(milliseconds=200))
            result = self._results.take(max(wait, datetime.timedelta(0)))
            if result is not None:
                if result.goal_id() == goal_id:
                    return result
                # 不是这个目标的结果，继续等。
        return None

    def destroy(self):
        delete_participant(self._participant)
