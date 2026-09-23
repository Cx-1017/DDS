# 第 1 章 · 裸 Fast DDS API（Python）

这一章**刻意不用任何自定义封装**，把 Fast DDS 实体的创建流程完整摊开。
本目录与 `cpp/01_basic_api/` 逐行对应，可以对照着读。

## 文件说明

| 文件 | 作用 |
|---|---|
| `BasicTypes.idl` | 数据类型定义，与 `cpp/01_basic_api/BasicTypes.idl` 内容完全相同。生成出 `BasicTypes.py` 模块 |
| `publisher.py` | **发布端**。按注释走一遍 `DomainParticipantFactory` → `create_participant` → `register_type` → `create_topic` → `create_publisher` → `create_datawriter` → `write` 的完整链路 |
| `subscriber_listener.py` | **订阅端（listener 回调）**。`create_datareader` 传 listener 对象，在 `on_data_available` 里取数据。回调跑在 DDS 内部线程上 |
| `subscriber_polling.py` | **订阅端（主线程轮询）**。`create_datareader` 传 `None`，主循环用 `wait_for_unread_message()` 阻塞等待。控制流全在 `main` 里 |
| `CMakeLists.txt` | 生成类型代码 + 用 SWIG 包成可 import 的 Python 模块 |

## 生成代码怎么被 import 进来

`BasicTypes.py` 是构建产物，不在源码目录里。每个脚本开头的三行就是在指路：

```python
_REPO_ROOT = Path(__file__).resolve().parents[2]     # python/<章节>/x.py → 仓库根
sys.path.insert(0, str(_REPO_ROOT / "build" / "python" / "01_basic_api"))
import BasicTypes
```

**这不是共享封装**，只是「生成代码的存放位置」。所以：
必须先构建，再运行；而且每章只用自己那份生成代码（各章的模块名不同，不会串）。

另外注意区分两件事 —— 生成的文件名跟着 `.idl` 文件名走，
里面的类型名跟着 IDL 里的 `struct` 名走：
`BasicTypes.idl` → 模块 `BasicTypes.py`，但类型叫 `BasicTypes.HelloWorld`。

## 与 C++ 的三点用法差异

1. **每个实体由它的"父实体"创建**，而不是各自 `new`。
2. **QoS 不能直接写 `*_QOS_DEFAULT` 哨兵**，要先造一个空的 Qos 对象、
   用 `get_default_xxx_qos()` 填充它，再传给 `create_xxx()`。
3. 改 QoS 的顺序同样是「取默认值 → 改字段 → 用它创建」，
   因为 `get_default_xxx_qos()` 是整体赋值而不是合并字段。

## 两个容易踩的坑

**1. `wait_for_unread_message()` 返回 `bool` 不是 `ReturnCode_t`。**
它返回"有数据 `True` / 超时 `False`"。写成
`if reader.wait_for_unread_message(t) != fastdds.RETCODE_OK:` 的话，
因为 `RETCODE_OK` 是 0 而 `True` 是 1，判断恒成立 ——
结果是**每次有数据都跳过**，一条都收不到。这个 bug 不报错，只是静默地什么都不打印。

**2. listener 对象不能被垃圾回收。**
`create_datareader(topic, qos, listener)` 之后，reader 持有 listener 的引用，
并在自己的线程里调用它。如果这个 Python 对象被 GC 回收了，
回调会**静默失效** —— 不报错，只是再也收不到数据。
所以脚本里都把它绑在一个活到程序结束的变量上。

## 关于官方示例

eProsima 官方的 `HelloWorldExample.py` 里有两个常见错误，本项目修掉了，
并在代码注释里标了出来，可以作为反面教材对照：

- `on_data_available` 只取一条样本，没有 `while` 排空 —— 数据积压时处理不过来；
- 没有检查 `info.valid_data` —— 会把 dispose/unregister 这类生命周期事件
  当成正常消息打印出来。

## 构建与运行

```zsh
# 先保证 fastdds 绑定在 PYTHONPATH 上（见仓库根 README）
export PYTHONPATH="$HOME/Fast-DDS-python/install/fastdds_python/lib/python3.10/site-packages${PYTHONPATH:+:$PYTHONPATH}"

cmake --build build --target BasicTypesWrapper
```

- 终端 A（先起）：`python3 python/01_basic_api/subscriber_listener.py`
- 终端 A（可选）：`python3 python/01_basic_api/subscriber_polling.py`
- 终端 B：`python3 python/01_basic_api/publisher.py`
