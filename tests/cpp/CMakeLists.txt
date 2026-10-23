cmake_minimum_required(VERSION 3.20)

# C++ 集成测试 — 使用项目自身的 mcp_client 测试 mcp_server
add_executable(test_mcp_integration test_mcp_integration.cpp)

# AI 模式需要 jsoncpp 和 libcurl（mcp_client 中是 PRIVATE 链接，不会传递）
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
find_package(CURL REQUIRED)

target_link_libraries(test_mcp_integration PRIVATE
    mcp_client
    mcp_common
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
)

target_include_directories(test_mcp_integration PRIVATE
    ${JSONCPP_INCLUDE_DIRS}
)

target_compile_features(test_mcp_integration PRIVATE cxx_std_20)

# 独立的在线端到端场景：集成层 + C++ RAG + stdio Tool + 外部模型。
add_executable(test_rag_agent_e2e test_rag_agent_e2e.cpp)
target_link_libraries(test_rag_agent_e2e PRIVATE mcp_client CURL::libcurl)
target_compile_features(test_rag_agent_e2e PRIVATE cxx_std_20)

# Offline transport/lifecycle regression tests; no model or weather API required.
add_executable(test_mcp_timeouts test_mcp_timeouts.cpp)
target_link_libraries(test_mcp_timeouts PRIVATE mcp_client Threads::Threads)
target_include_directories(test_mcp_timeouts PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_compile_features(test_mcp_timeouts PRIVATE cxx_std_20)
add_dependencies(test_mcp_timeouts mcp_server)
add_test(NAME mcp_timeouts COMMAND test_mcp_timeouts
    $<TARGET_FILE:mcp_server> ${CMAKE_BINARY_DIR}/mcp_server/plugins)
set_tests_properties(mcp_timeouts PROPERTIES TIMEOUT 30)

add_executable(test_rag_mmr test_rag_mmr.cpp)
target_link_libraries(test_rag_mmr PRIVATE mcp_client)
target_compile_features(test_rag_mmr PRIVATE cxx_std_20)
add_test(NAME rag_mmr COMMAND test_rag_mmr)
set_tests_properties(rag_mmr PROPERTIES TIMEOUT 10)

add_executable(test_rag_hybrid test_rag_hybrid.cpp)
target_link_libraries(test_rag_hybrid PRIVATE mcp_client)
target_compile_features(test_rag_hybrid PRIVATE cxx_std_20)
add_test(NAME rag_hybrid COMMAND test_rag_hybrid)
set_tests_properties(rag_hybrid PROPERTIES TIMEOUT 10)
