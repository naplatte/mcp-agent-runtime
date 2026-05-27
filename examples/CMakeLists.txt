cmake_minimum_required(VERSION 3.20)

# RAG-MCP 示例
add_executable(rag_mcp_example rag_mcp_example.cpp)
target_link_libraries(rag_mcp_example PRIVATE
    mcp_client
    mcp_common
)
target_compile_features(rag_mcp_example PRIVATE cxx_std_20)
