# Fast DDS 与 ROS 2 学习路径

用 Fast DDS 从零实现 ROS 2 的几种通信原语。每一章先看 DDS 怎么把这件事拼出来，
再看封装成 ROS 风格 API 之后长什么样。

## 目录结构

按 **语言 → 章节** 两级组织。每个章节目录都是自包含的：自带 `.idl` 类型定义、
自带的构建脚本、自带的 README，章与章之间**没有共享的封装代码**，
单独打开任意一个目录都能读懂全貌。

```
fastdds_ros_learning/
├── CMakeLists.txt          顶层：find_package + 引入下面两个语言目录
├── cpp/                    C++ 实现
│   ├── 01_basic_api/       裸 Fast DDS API —— 创建流程 + 两种取数方式
│   ├── 02_service/         用两条 Topic 拼出 ROS 2 风格的服务
│   ├── 03_topic/           用 DDS Topic 拼出 rclcpp 风格的发布订阅
│   ├── 04_parameter/       参数声明/读取/设置 + 参数事件
│   └── 05_action/          目标/反馈/结果/取消四条通道
└── python/                 与 cpp/ 逐章对应的 Python 实现
    ├── 01_basic_api/
    ├── 02_service/
    ├── 03_topic/
    ├── 04_parameter/
    └── 05_action/
```

两个语言目录里的 `.idl` 是**内容相同的副本**。这样做是为了让两边都能单独阅读；
因为它们内容一致，生成出的类型在 DDS 线路上是二进制兼容的 ——
C++ 的客户端可以直接调用 Python 的服务端，反之亦然（这一点已验证过）。

各章的具体文件说明见该章节目录下的 `README.md`。

## 学习顺序

| 章节 | DDS 侧学什么 | ROS 2 对应物 |
|---|---|---|
| 01 | `DomainParticipant` → `TypeSupport` → `Topic` → `DataWriter`/`DataReader` 的完整创建流程；listener 回调与主动轮询两种取数方式 | 无（纯 DDS） |
| 02 | 两条 Topic + `request_id` 配对，在无 RPC 的中间件上实现请求-响应 | `create_service` / `create_client` |
| 03 | Topic 名 + 类型名 + QoS 决定匹配；回调运行在 DDS 线程上 | `create_publisher` / `create_subscription` |
| 04 | 服务骨架 + 一条单向广播通道（`parameter_events`）；IDL `enum` 的用法 | `declare_parameter` / `get_parameter` / `set_parameter` |
| 05 | 长时间存活的多通道会话：反馈和取消是独立 Topic，靠 `goal_id` 关联 | `rclcpp_action` / `rclpy.action` |

**第 1 章是关键**：它不用任何自定义类，把所有 DDS 实体的创建过程摊开写了一遍。
第 2~5 章的封装类里做的事，本质都是第 1 章那套流程，只是换成了 ROS 的 API 形状。

## 环境

按当前机器上的配置编写，需要：

- **Fast DDS 3.x** —— 默认装在 `$HOME/Fast-DDS/install/fastdds`
- **Fast DDS Python bindings** —— 默认装在 `$HOME/Fast-DDS-python/install/fastdds_python`
- **fastddsgen** —— 把 `.idl` 翻译成 C++ 类型代码和 Python 绑定
- **SWIG**、**CMake ≥ 3.20**、**Python 3.10**、JDK（fastddsgen 依赖）

路径不同的话，用 `FASTDDS_HOME` / `FASTDDSGEN` 两个环境变量覆盖。
**不要同时 source 多套不同版本的 Fast DDS**，会出现很诡异的类型不匹配。

## 构建

从本目录（`fastdds_ros_learning/`）执行：

```zsh
# 1. 让运行时能找到 Fast DDS 的库
source "$HOME/Fast-DDS/install/setup.zsh"
source "$HOME/Fast-DDS-python/install/setup.zsh"

# 2. 配置并构建（会顺带为 10 个 .idl 生成代码）
cmake -S . -B build \
    -DFASTDDS_HOME="$HOME/Fast-DDS/install/fastdds" \
    -DFASTDDSGEN="$HOME/Fast-DDS/src/fastddsgen/scripts/fastddsgen"
cmake --build build -j4

# 3. Python 脚本还要能找到 fastdds 绑定
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"
```

只构建某一章：`cmake --build build --target <该章的程序名>`，例如
`cmake --build build --target service_server`。

生成的代码都在 `build/` 下，按语言和章节分开：

```
build/cpp/<章节>/generated/    fastddsgen 生成的 C++ 类型代码
build/cpp/<章节>/<程序名>       可执行文件
build/python/<章节>/<X>Types.py   Python 类型模块（各章脚本 import 的就是它）
build/python/<章节>/_<X>TypesWrapper.so   上面那个模块背后的 C++ 扩展
```

各章的 Python 脚本会用 `sys.path.insert` 指向自己那一章的生成目录 ——
所以**必须先构建再运行**，且每章只用自己那份生成代码。

改了 `.idl` 之后重新 `cmake --build build` 即可（构建脚本带了 `-replace`，
会覆盖旧文件；如果换成不带 `-replace` 的写法，fastddsgen 会跳过已存在的文件，
让你一直编译到陈旧代码）。

## 运行

每一组都要开两个终端：先起服务端/订阅端，再起客户端/发布端。
所有程序都在 DDS **域 0**，同一章内 Topic 名和类型名一致才能互相发现。

### C++

| 章节 | 终端 A（先起） | 终端 B |
|---|---|---|
| 01 发布订阅 | `./build/cpp/01_basic_api/basic_subscriber_listener` | `./build/cpp/01_basic_api/basic_publisher` |
| 01 轮询取数 | `./build/cpp/01_basic_api/basic_subscriber_polling` | （同上，可以和上一个同时跑） |
| 02 服务 | `./build/cpp/02_service/service_server` | `./build/cpp/02_service/service_client` |
| 03 话题 | `./build/cpp/03_topic/topic_listener` | `./build/cpp/03_topic/topic_talker` |
| 04 参数 | `./build/cpp/04_parameter/param_server` | `./build/cpp/04_parameter/param_client` |
| 04 参数事件 | `./build/cpp/04_parameter/param_monitor`（第三个终端） | （配合上面那条，改参数时会收到通知） |
| 05 action | `./build/cpp/05_action/action_server` | `./build/cpp/05_action/action_client` |
| 05 取消 | （同上） | `./build/cpp/05_action/action_client cancel` |

### Python

先设好上面的 `PYTHONPATH`，再从本目录执行：

| 章节 | 终端 A（先起） | 终端 B |
|---|---|---|
| 01 发布订阅 | `python3 python/01_basic_api/subscriber_listener.py` | `python3 python/01_basic_api/publisher.py` |
| 01 轮询取数 | `python3 python/01_basic_api/subscriber_polling.py` | （同上） |
| 02 服务 | `python3 python/02_service/server.py` | `python3 python/02_service/client.py` |
| 03 话题 | `python3 python/03_topic/listener.py` | `python3 python/03_topic/talker.py` |
| 04 参数 | `python3 python/04_parameter/server.py` | `python3 python/04_parameter/client.py` |
| 04 参数事件 | `python3 python/04_parameter/monitor.py`（第三个终端） | （配合上面那条） |
| 05 action | `python3 python/05_action/server.py` | `python3 python/05_action/client.py` |
| 05 取消 | （同上） | `python3 python/05_action/client.py cancel` |

### 值得动手试的几件事

- **一对多 / 多对多**：同时开两个订阅端（listener 和 polling 各一个），
  再开一个发布端，两边都能收到全部 10 条；开两个发布端则每个订阅端收到 20 条。
- **跨语言互通**：C++ 的 `service_client` 调 Python 的 `server.py`，
  或者反过来。两端生成的类型是二进制兼容的，能直接通信。
- **观察发现过程**：另开一个终端跑 `$HOME/Fast-DDS-python/install/fastdds/bin/fastdds discovery -i 0 -l`，
  能看到各章实体是怎么被互相发现的。

## 与 ROS 2 的关系

ROS 2 用 DDS 做 RMW 中间件，但**这里的程序不能和 rclcpp/rclpy 节点互通**。
ROS 2 另外规定了一整套约定：消息类型映射、topic/service 命名规则、
QoS 配置的语义、服务请求的关联方式、参数接口、action 状态机……

本项目的第 2、4、5 章是**教学用的轻量协议**：保留了 ROS 的调用习惯和通信方向，
但用的是自己定义的 IDL 类型和 Topic 名，没有实现 ROS 的 wire protocol。
它们的价值在于把"ROS 的某个概念，在 DDS 上到底是怎么用 Topic 拼出来的"
这件事讲清楚 —— 理解了这层，再去看 ROS 2 的 RMW 实现和 QoS 文档会顺畅很多。
