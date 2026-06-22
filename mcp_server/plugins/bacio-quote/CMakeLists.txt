cmake_minimum_required(VERSION 3.10)

# Bacio-quote 插件库
add_library(bacio-quote SHARED
        ${PROJECT_SOURCE_DIR}/plugins/bacio-quote/BacioQuote.cpp
)

set_target_properties(bacio-quote PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(bacio-quote PRIVATE Threads::Threads)

target_compile_definitions(bacio-quote PRIVATE BACIOQUOTE_EXPORTS)
target_include_directories(bacio-quote PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/../third_party)
