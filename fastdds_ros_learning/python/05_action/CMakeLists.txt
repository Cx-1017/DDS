# 第 5 章 · Python 绑定构建
#
# 结构同前几章。这一章的 IDL 里有 4 个 struct
# （Goal / Feedback / Result / Cancel），一起包进 ActionTypes.py。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

execute_process(
    COMMAND "${FASTDDSGEN}" -replace -python -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ActionTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 ActionTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

add_library(action_py_types SHARED
    "${GEN_DIR}/ActionTypesPubSubTypes.cxx"
    "${GEN_DIR}/ActionTypesTypeObjectSupport.cxx")
target_include_directories(action_py_types PUBLIC "${GEN_DIR}")
target_link_libraries(action_py_types PUBLIC fastdds fastcdr)

# 目标名必须是 ActionTypesWrapper —— fastddsgen 在 ActionTypes.i 里
# 写死了模块导入名 _ActionTypesWrapper。
set(CMAKE_SWIG_OUTDIR "${GEN_DIR}")
set_source_files_properties("${GEN_DIR}/ActionTypes.i" PROPERTIES
    CPLUSPLUS ON
    USE_TARGET_INCLUDE_DIRECTORIES TRUE)

swig_add_library(ActionTypesWrapper TYPE MODULE LANGUAGE python SOURCES "${GEN_DIR}/ActionTypes.i")
target_include_directories(ActionTypesWrapper PRIVATE "${GEN_DIR}" ${Python3_INCLUDE_DIRS})
target_link_libraries(ActionTypesWrapper PRIVATE Python3::Module action_py_types fastdds)
set_target_properties(ActionTypesWrapper PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${GEN_DIR}")
