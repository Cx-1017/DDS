# Python 章节列表，与 cpp/ 逐章对应。
#
# Python 端的代码生成比 C++ 多一步 SWIG：.idl 先变成 C++ 类型代码，
# 再由 SWIG 包装成可 import 的 Python 模块。
#
# 生成物落在 build/python/<章节>/ 下，各章的 .py 脚本会把这个路径
# 加进 sys.path 后 import 本章的类型模块。

add_subdirectory(01_basic_api)
add_subdirectory(02_service)
add_subdirectory(03_topic)
add_subdirectory(04_parameter)
add_subdirectory(05_action)
