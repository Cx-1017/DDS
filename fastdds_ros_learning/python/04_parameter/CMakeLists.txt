# 第 4 章 · Python 绑定构建
#
# 结构同前几章。这一章的 IDL 里有 3 个 struct + 1 个 enum，
# 会一起被包进同一个 Python 模块 ParameterTypes.py。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

execute_process(
    COMMAND "${FASTDDSGEN}" -replace -python -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ParameterTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 ParameterTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

add_library(parameter_py_types SHARED
    "${GEN_DIR}/ParameterTypesPubSubTypes.cxx"
    "${GEN_DIR}/ParameterTypesTypeObjectSupport.cxx")
target_include_directories(parameter_py_types PUBLIC "${GEN_DIR}")
target_link_libraries(parameter_py_types PUBLIC fastdds fastcdr)

# 目标名必须是 ParameterTypesWrapper —— fastddsgen 在 ParameterTypes.i 里
# 写死了模块导入名 _ParameterTypesWrapper。
set(CMAKE_SWIG_OUTDIR "${GEN_DIR}")
set_source_files_properties("${GEN_DIR}/ParameterTypes.i" PROPERTIES
    CPLUSPLUS ON
    USE_TARGET_INCLUDE_DIRECTORIES TRUE)

swig_add_library(ParameterTypesWrapper TYPE MODULE LANGUAGE python SOURCES "${GEN_DIR}/ParameterTypes.i")
target_include_directories(ParameterTypesWrapper PRIVATE "${GEN_DIR}" ${Python3_INCLUDE_DIRS})
target_link_libraries(ParameterTypesWrapper PRIVATE Python3::Module parameter_py_types fastdds)
set_target_properties(ParameterTypesWrapper PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${GEN_DIR}")
