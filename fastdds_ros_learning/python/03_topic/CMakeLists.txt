# 第 3 章 · Python 绑定构建
#
# 结构同前两章，只是换了 .idl 文件名。

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(MAKE_DIRECTORY "${GEN_DIR}/tmp")   # 隔离 fastddsgen 的预处理中间文件

execute_process(
    COMMAND "${FASTDDSGEN}" -replace -python -flat-output-dir
            -t "${GEN_DIR}/tmp/"
            -d "${GEN_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/TopicTypes.idl"
    RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "fastddsgen 生成 TopicTypes.idl 失败，请确认 -DFASTDDSGEN= 指向可用的生成器")
endif()

add_library(topic_py_types SHARED
    "${GEN_DIR}/TopicTypesPubSubTypes.cxx"
    "${GEN_DIR}/TopicTypesTypeObjectSupport.cxx")
target_include_directories(topic_py_types PUBLIC "${GEN_DIR}")
target_link_libraries(topic_py_types PUBLIC fastdds fastcdr)

# 目标名必须是 TopicTypesWrapper —— fastddsgen 在 TopicTypes.i 里
# 写死了模块导入名 _TopicTypesWrapper。
set(CMAKE_SWIG_OUTDIR "${GEN_DIR}")
set_source_files_properties("${GEN_DIR}/TopicTypes.i" PROPERTIES
    CPLUSPLUS ON
    USE_TARGET_INCLUDE_DIRECTORIES TRUE)

swig_add_library(TopicTypesWrapper TYPE MODULE LANGUAGE python SOURCES "${GEN_DIR}/TopicTypes.i")
target_include_directories(TopicTypesWrapper PRIVATE "${GEN_DIR}" ${Python3_INCLUDE_DIRS})
target_link_libraries(TopicTypesWrapper PRIVATE Python3::Module topic_py_types fastdds)
set_target_properties(TopicTypesWrapper PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${GEN_DIR}")
