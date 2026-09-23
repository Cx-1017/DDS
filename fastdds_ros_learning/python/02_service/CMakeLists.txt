# 第 2 章 · Python 绑定构建
#
# 和第 1 章的构建脚本结构相同，只是换了 .idl 文件名。
# ServiceTypes.idl 里有两个 struct（AddRequest / AddResponse），
# 它们会一起被包进同一个 Python 模块 ServiceTypes.py。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

execute_process(
    COMMAND "${FASTDDSGEN}" -replace -python -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ServiceTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 ServiceTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

# ── 类型库：SWIG 生成的扩展要链接它 ───────────────────────────────────────
add_library(service_py_types SHARED
    "${GEN_DIR}/ServiceTypesPubSubTypes.cxx"
    "${GEN_DIR}/ServiceTypesTypeObjectSupport.cxx")
target_include_directories(service_py_types PUBLIC "${GEN_DIR}")
target_link_libraries(service_py_types PUBLIC fastdds fastcdr)

# ── SWIG：把 C++ 类型暴露成 Python 模块 ────────────────────────────────────
# 目标名必须是 ServiceTypesWrapper —— fastddsgen 在 ServiceTypes.i 里
# 写死了模块导入名 _ServiceTypesWrapper。
set(CMAKE_SWIG_OUTDIR "${GEN_DIR}")
set_source_files_properties("${GEN_DIR}/ServiceTypes.i" PROPERTIES
    CPLUSPLUS ON
    USE_TARGET_INCLUDE_DIRECTORIES TRUE)

swig_add_library(ServiceTypesWrapper TYPE MODULE LANGUAGE python SOURCES "${GEN_DIR}/ServiceTypes.i")
target_include_directories(ServiceTypesWrapper PRIVATE "${GEN_DIR}" ${Python3_INCLUDE_DIRS})
target_link_libraries(ServiceTypesWrapper PRIVATE Python3::Module service_py_types fastdds)
set_target_properties(ServiceTypesWrapper PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${GEN_DIR}")
