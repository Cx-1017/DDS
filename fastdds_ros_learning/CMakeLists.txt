# Fast DDS 学习项目 · 顶层构建
#
# 目录按"语言 → 章节"两级组织，每个章节在自己的目录里自带 .idl、
# 自带代码生成、自带可执行文件，章与章之间没有共享的封装代码。
#
#   cpp/<章节>/       C++ 实现
#   python/<章节>/    Python 实现（同样的章节划分）
#
# 详细用法见 README.md。

cmake_minimum_required(VERSION 3.20)
project(fastdds_ros_learning LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fast DDS 的安装前缀。默认是 $HOME/Fast-DDS/install/fastdds，
# 可以用 -DFASTDDS_HOME=/别的/路径 覆盖。
set(FASTDDS_HOME "$ENV{HOME}/Fast-DDS/install/fastdds" CACHE PATH "Fast DDS install prefix")

# fastddsgen 是把 .idl 翻译成 C++ 类型代码和 Python 绑定的生成器。
set(FASTDDSGEN "$ENV{HOME}/Fast-DDS/src/fastddsgen/scripts/fastddsgen" CACHE FILEPATH "fastddsgen executable")

# find_package 只会在 CMAKE_PREFIX_PATH 里找 Fast DDS，
# 所以要把上面那个前缀塞进去。
list(PREPEND CMAKE_PREFIX_PATH "${FASTDDS_HOME}")
find_package(fastdds 3 REQUIRED)   # Fast DDS 本体，版本 3.x
find_package(fastcdr REQUIRED)     # 配套的 CDR 序列化库，跟着 Fast DDS 一起装
find_package(SWIG REQUIRED)        # 用来把 C++ 类型包成 Python 模块
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# swig_add_library() 由这个模块提供，必须在顶层 include 一次，
# 子目录才能直接用。
include(UseSWIG)

add_subdirectory(cpp)
add_subdirectory(python)
