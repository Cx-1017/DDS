# C++ 章节列表。
#
# 每一章都是独立的一课：自己的 .idl、自己的代码生成、自己的可执行文件。
# 想只构建某一章，直接 cmake --build build --target <该章的可执行文件名>。
#
# 新增章节时，把目录建好，然后在下面加一行 add_subdirectory 即可。

add_subdirectory(01_basic_api)   # 裸 Fast DDS API：创建流程 + 两种取数方式
add_subdirectory(02_service)     # 用两条 Topic 拼出 ROS 2 风格的服务
add_subdirectory(03_topic)       # 用 DDS Topic 拼出 rclcpp 风格的发布订阅
add_subdirectory(04_parameter)   # 参数声明/读取/设置 + 参数事件
add_subdirectory(05_action)      # 目标/反馈/结果/取消四条通道
