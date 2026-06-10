cmake_minimum_required(VERSION 3.20)

# C++ 集成测试 — 使用项目自身的 mcp_client 测试 mcp_server
add_executable(test_mcp_integration test_mcp_integration.cpp)

target_link_libraries(test_mcp_integration PRIVATE
    mcp_client
    mcp_common
)

target_compile_features(test_mcp_integration PRIVATE cxx_std_20)
