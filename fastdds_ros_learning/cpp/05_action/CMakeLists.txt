# 第 5 章 · 用 DDS 实现 ROS 2 风格的 action
#
# 构建结构同前几章。这一章的 IDL 里有 4 个 struct
# （Goal / Feedback / Result / Cancel），一起生成到同一组文件里。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

# -d 指定的目录必须已存在，-replace 不能省。
execute_process(
    COMMAND "${FASTDDSGEN}" -replace -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ActionTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 ActionTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

add_library(action_types SHARED
    "${GEN_DIR}/ActionTypesPubSubTypes.cxx"
    "${GEN_DIR}/ActionTypesTypeObjectSupport.cxx")
target_include_directories(action_types PUBLIC "${GEN_DIR}")
target_link_libraries(action_types PUBLIC fastdds fastcdr)

# 目标名带 action_ 前缀，避免和其它章节的同名程序冲突。
add_executable(action_server "${CMAKE_CURRENT_SOURCE_DIR}/server.cpp")
add_executable(action_client "${CMAKE_CURRENT_SOURCE_DIR}/client.cpp")

foreach(APP IN ITEMS action_server action_client)
    target_include_directories(${APP} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${APP} PRIVATE action_types fastdds)
endforeach()
