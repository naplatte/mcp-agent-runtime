cmake_minimum_required(VERSION 3.10)

# Calculator plugin library
add_library(calculator SHARED
    ${PROJECT_SOURCE_DIR}/plugins/calculator/Calculator.cpp
)

set_target_properties(calculator PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(calculator PRIVATE Threads::Threads)

target_compile_definitions(calculator PRIVATE CALCULATOR_EXPORTS)
target_include_directories(calculator PRIVATE 
    ${PROJECT_SOURCE_DIR}/include 
    ${PROJECT_SOURCE_DIR}/src/interface
)
