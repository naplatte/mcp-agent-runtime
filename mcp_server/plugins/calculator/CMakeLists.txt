cmake_minimum_required(VERSION 3.10)

# 目标是创建一个共享库，shared即创建动态链接库.so文件
add_library(calculator SHARED
    ${PROJECT_SOURCE_DIR}/plugins/calculator/Calculator.cpp # 源文件
)

# 设置编译属性 - 启用位置无关代码（相对寻址）
set_target_properties(calculator PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 链接第三方库
find_package(Threads REQUIRED)
target_link_libraries(calculator PRIVATE Threads::Threads)

# 添加预处理器定义
target_compile_definitions(calculator PRIVATE CALCULATOR_EXPORTS)

# 为 calculator 目标添加头文件搜索路径
target_include_directories(calculator PRIVATE 
    ${PROJECT_SOURCE_DIR}/include 
    ${PROJECT_SOURCE_DIR}/../third_party
)
