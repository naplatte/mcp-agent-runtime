cmake_minimum_required(VERSION 3.20)

# 公共组件库（轻量版 - 仅包含 MCP 所需的 logger）
set(COMMON_SOURCES
    src/logger.cpp
)

# 将 common 源文件 logger.cpp 编译为一个静态库
add_library(mcp_common STATIC ${COMMON_SOURCES}) 

# 设置头文件目录 common/include
target_include_directories(mcp_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(mcp_common PRIVATE cxx_std_20)

# 开启常见警告
target_compile_options(mcp_common PRIVATE -Wall -Wextra)

# 安装静态库到 lib 目录
install(TARGETS mcp_common
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# 安装 agent_rpc 下的 .h 头文件到 include 目录
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/agent_rpc
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
