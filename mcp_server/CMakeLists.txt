cmake_minimum_required(VERSION 3.20)
project(mcp_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 设置版本号0.7.0
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 7)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

# 生成 version.h
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
)

# 查找线程库
find_package(Threads REQUIRED)

# 源文件
set(SOURCES
    src/main.cpp
    src/Server.cpp
    src/StdioTransport.cpp
    src/StreamableHttpTransport.cpp
    src/PluginsLoader.cpp
)

# 生成一个可执行文件（和client不同，client最终是编译出了一个静态库）
add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${PROJECT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_BINARY_DIR}
)

# mcp_server链接线程库
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# 平台相关链接
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_options(${PROJECT_NAME} PRIVATE "-static-libgcc" "-static-libstdc++")
endif()

# 编译多个示例插件
add_subdirectory(plugins/weather)
add_subdirectory(plugins/sleep)
add_subdirectory(plugins/code-review)
add_subdirectory(plugins/bacio-quote)
add_subdirectory(plugins/notification)
add_subdirectory(plugins/calculator)
