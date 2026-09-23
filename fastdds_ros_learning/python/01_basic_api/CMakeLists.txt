# 第 1 章 · Python 绑定构建
#
# 和 C++ 那章的区别只有两点：
#   1. fastddsgen 要多加 -python，额外生成 SWIG 接口文件 BasicTypes.i
#   2. 多了 swig_add_library() 这一步，把 C++ 类型包成可 import 的 Python 模块
#
# 生成物全部落在 build/python/01_basic_api/ 下：
#   BasicTypes.py          —— Python 侧的类型定义（本目录的 .py 脚本 import 的就是它）
#   _BasicTypesWrapper.so  —— 上面那个模块背后的 C++ 扩展
# 本章的脚本用 sys.path.insert 指向这个目录，路径写在各脚本开头。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

execute_process(
    COMMAND "${FASTDDSGEN}" -replace -python -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/BasicTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 BasicTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

# ── 类型库：SWIG 生成的扩展要链接它 ───────────────────────────────────────
add_library(basic_py_types SHARED
    "${GEN_DIR}/BasicTypesPubSubTypes.cxx"
    "${GEN_DIR}/BasicTypesTypeObjectSupport.cxx")
target_include_directories(basic_py_types PUBLIC "${GEN_DIR}")
target_link_libraries(basic_py_types PUBLIC fastdds fastcdr)

# ── SWIG：把 C++ 类型暴露成 Python 模块 ────────────────────────────────────
# 【重要】目标名必须叫 BasicTypesWrapper。
# fastddsgen 在生成的 BasicTypes.i 里写死了模块导入名 _BasicTypesWrapper
# （见 BasicTypes.i 开头的 %module(moduleimport=...)），改名字会导致 import 失败。
# 规律：目标名 = IDL 文件名的驼峰形式 + "Wrapper"。
set(CMAKE_SWIG_OUTDIR "${GEN_DIR}")
set_source_files_properties("${GEN_DIR}/BasicTypes.i" PROPERTIES
    CPLUSPLUS ON
    USE_TARGET_INCLUDE_DIRECTORIES TRUE)

swig_add_library(BasicTypesWrapper TYPE MODULE LANGUAGE python SOURCES "${GEN_DIR}/BasicTypes.i")
target_include_directories(BasicTypesWrapper PRIVATE "${GEN_DIR}" ${Python3_INCLUDE_DIRS})
target_link_libraries(BasicTypesWrapper PRIVATE Python3::Module basic_py_types fastdds)
set_target_properties(BasicTypesWrapper PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${GEN_DIR}")
