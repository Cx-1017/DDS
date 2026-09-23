# 第 4 章 · 用 DDS 实现 ROS 2 风格的参数系统
#
# 构建结构同前几章：生成类型代码 → 编译成库 → 编译可执行文件。
# 这一章的 IDL 里有 3 个 struct + 1 个 enum，都生成到同一组文件里。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

# -d 指定的目录必须已存在，-replace 不能省。
execute_process(
    COMMAND "${FASTDDSGEN}" -replace -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ParameterTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 ParameterTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

add_library(parameter_types SHARED
    "${GEN_DIR}/ParameterTypesPubSubTypes.cxx"
    "${GEN_DIR}/ParameterTypesTypeObjectSupport.cxx")
target_include_directories(parameter_types PUBLIC "${GEN_DIR}")
target_link_libraries(parameter_types PUBLIC fastdds fastcdr)

# 目标名带 param_ 前缀，避免和其它章节的同名程序冲突（CMake 目标名全局唯一）。
add_executable(param_server  "${CMAKE_CURRENT_SOURCE_DIR}/server.cpp")
add_executable(param_client  "${CMAKE_CURRENT_SOURCE_DIR}/client.cpp")
add_executable(param_monitor "${CMAKE_CURRENT_SOURCE_DIR}/monitor.cpp")

foreach(APP IN ITEMS param_server param_client param_monitor)
    target_include_directories(${APP} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${APP} PRIVATE parameter_types fastdds)
endforeach()
