cmake_minimum_required(VERSION 3.10)

add_library(log_analyzer SHARED
    ${PROJECT_SOURCE_DIR}/plugins/log-analyzer/LogAnalyzer.cpp
)

set_target_properties(log_analyzer PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(log_analyzer PRIVATE Threads::Threads)

target_compile_definitions(log_analyzer PRIVATE LOG_ANALYZER_EXPORTS)

target_include_directories(log_analyzer PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/../third_party
)
