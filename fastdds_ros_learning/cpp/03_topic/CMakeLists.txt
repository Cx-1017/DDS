# 第 3 章 · 把 DDS Topic 包成 ROS 2 风格的发布订阅
#
# 构建结构同前两章：生成类型代码 → 编译成库 → 编译可执行文件。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

# -d 指定的目录必须已存在，-replace 不能省。
execute_process(
    COMMAND "${FASTDDSGEN}" -replace -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/TopicTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 TopicTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

add_library(topic_types SHARED
    "${GEN_DIR}/TopicTypesPubSubTypes.cxx"
    "${GEN_DIR}/TopicTypesTypeObjectSupport.cxx")
target_include_directories(topic_types PUBLIC "${GEN_DIR}")
target_link_libraries(topic_types PUBLIC fastdds fastcdr)

# node.hpp 是纯头文件封装，两个程序各自 include。
# 目标名带 topic_ 前缀，避免和其它章节的同名程序冲突。
add_executable(topic_talker   "${CMAKE_CURRENT_SOURCE_DIR}/talker.cpp")
add_executable(topic_listener "${CMAKE_CURRENT_SOURCE_DIR}/listener.cpp")

foreach(APP IN ITEMS topic_talker topic_listener)
    target_include_directories(${APP} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${APP} PRIVATE topic_types fastdds)
endforeach()
