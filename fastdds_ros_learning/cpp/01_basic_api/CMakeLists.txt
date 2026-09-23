# 第 1 章 · 裸 Fast DDS API
#
# 这一章刻意不使用任何自定义封装类，所以这里的构建也最简单：
# 生成类型代码 → 编译成库 → 把三个 .cpp 各自编译成可执行文件。

# ── 1. 把 BasicTypes.idl 翻译成 C++ 类型代码 ───────────────────────────────
# fastddsgen 会生成：
#   BasicTypes.hpp                数据结构 HelloWorld
#   BasicTypesPubSubTypes.{hpp,cxx}   DDS 类型适配类（序列化/反序列化）
#   BasicTypesTypeObjectSupport.{hpp,cxx}  类型的自描述信息，用于发现阶段
#
# 生成的文件名跟着 .idl 的文件名走（BasicTypes.idl → BasicTypes*.{hpp,cxx}），
# 而里面的类型名跟着 IDL 里的 struct 名走（struct HelloWorld → HelloWorld）。
# 两者是独立的，别搞混。
#
# 两个容易踩的坑：
#   * -d 指定的输出目录必须【已经存在】，否则 fastddsgen 直接报错退出，
#     所以下面要先 file(MAKE_DIRECTORY)。
#   * -replace 不能省。否则重配置时 fastddsgen 看到文件已存在会打印
#     "Skipping" 直接跳过，你会一直编译到上一次的陈旧代码。
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

# fastddsgen 的预处理中间文件默认写在 /tmp/<idl文件名>.cc，是全局固定路径。
# 用 -t 把它关到本章的构建目录里，避免不同章节的同名 IDL 互相覆盖。
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")

execute_process(
    COMMAND "${FASTDDSGEN}" -replace -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/BasicTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 BasicTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

# ── 2. 把生成的数据类型编译成一个共享库 ────────────────────────────────────
# 三个可执行文件共用它，避免每个都重复编译一遍类型代码。
add_library(basic_types SHARED
    "${GEN_DIR}/BasicTypesPubSubTypes.cxx"
    "${GEN_DIR}/BasicTypesTypeObjectSupport.cxx")
target_include_directories(basic_types PUBLIC "${GEN_DIR}")
target_link_libraries(basic_types PUBLIC fastdds fastcdr)

# ── 3. 三个可执行文件 ──────────────────────────────────────────────────────
# 目标名全局唯一，所以都带 basic_ 前缀，避免和其它章节的同名程序冲突。
#   basic_publisher            发布端
#   basic_subscriber_listener  订阅端之一：listener 回调
#   basic_subscriber_polling   订阅端之二：主动轮询
foreach(APP IN ITEMS basic_publisher basic_subscriber_listener basic_subscriber_polling)
    add_executable(${APP} "${CMAKE_CURRENT_SOURCE_DIR}/${APP}.cpp")
    target_link_libraries(${APP} PRIVATE basic_types fastdds)
endforeach()
