cmake_minimum_required(VERSION 3.10)
project(adaptive_grading LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译优化选项
if(MSVC)
    add_compile_options(/O2 /W3 /fp:fast)
else()
    add_compile_options(-O3 -Wall -Wextra -ffast-math)
    if(ANDROID)
        # Android NDK ARM64-v8a NEON 自动向量化
        add_compile_options(-march=armv8-a)
    endif()
endif()

# 核心静态库与动态库源码
set(CORE_SOURCES
    src/adaptive_grading.c
    src/adaptive_grading_neon.c
    src/adaptive_logger.c
    src/adaptive_storage.c
)

add_library(adaptive_grading_static STATIC
    ${CORE_SOURCES}
)
target_include_directories(adaptive_grading_static PUBLIC
    include
)

add_library(adaptive_grading SHARED
    ${CORE_SOURCES}
)
target_include_directories(adaptive_grading PUBLIC
    include
)

# 查找数学库
if(UNIX AND NOT ANDROID)
    target_link_libraries(adaptive_grading m)
    target_link_libraries(adaptive_grading_static m)
endif()

