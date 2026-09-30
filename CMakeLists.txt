cmake_minimum_required(VERSION 3.20)
project(MCP_Standalone VERSION 1.0.0 LANGUAGES CXX) # 模块名：MCP_Standalone

set(CMAKE_CXX_STANDARD 20) # C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 必须使用C++20,若编译器不支持 不会自动降版本
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 让 CMake 生成一个文件compile_commands.json

# 查找项目需要的外部依赖
find_package(PkgConfig REQUIRED) # 查找第三方库
find_package(CURL REQUIRED) # 查找curl库
find_package(Threads REQUIRED) # 查找线程库

# 公共组件
add_subdirectory(common)

# MCP 客户端库（含 RAG-MCP）
add_subdirectory(mcp_client)

# MCP 服务器
add_subdirectory(mcp_server)

# 是否编译 MCP 示例程序
option(BUILD_MCP_EXAMPLES "Build MCP examples" ON) 
if(BUILD_MCP_EXAMPLES)
    add_subdirectory(examples)
endif()

# 是否编译测试程序
option(BUILD_TESTS "Build C++ test programs" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/cpp)
endif()
