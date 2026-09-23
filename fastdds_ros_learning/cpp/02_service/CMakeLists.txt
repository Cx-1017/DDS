# 第 2 章 · 用 DDS 实现 ROS 2 风格的服务
#
# 构建方式和第 1 章完全一样：生成类型代码 → 编译成库 → 编译可执行文件。
# 区别只是这一章的 .idl 里有两个 struct（AddRequest / AddResponse），
# 它们会生成到同一组文件 Service.hpp / ServicePubSubTypes.hpp 里。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

# -d 指定的目录必须已存在，-replace 不能省（否则重复配置时会跳过已存在的文件）。
execute_process(
    COMMAND "${FASTDDSGEN}" -replace -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ServiceTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 ServiceTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

# 两个 struct 的 DDS 类型适配代码都在同一个 .cxx 里，编译一次即可。
add_library(service_types SHARED
    "${GEN_DIR}/ServiceTypesPubSubTypes.cxx"
    "${GEN_DIR}/ServiceTypesTypeObjectSupport.cxx")
target_include_directories(service_types PUBLIC "${GEN_DIR}")
target_link_libraries(service_types PUBLIC fastdds fastcdr)

# service.hpp 是纯头文件的封装，两个可执行文件各自 include 即可。
# 目标名带 service_ 前缀（源文件仍叫 server.cpp / client.cpp），
# 避免和其它章节的同名程序冲突 —— CMake 的目标名是全局唯一的。
add_executable(service_server "${CMAKE_CURRENT_SOURCE_DIR}/server.cpp")
add_executable(service_client "${CMAKE_CURRENT_SOURCE_DIR}/client.cpp")

foreach(APP IN ITEMS service_server service_client)
    target_include_directories(${APP} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${APP} PRIVATE service_types fastdds)
endforeach()
