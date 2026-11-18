cmake_minimum_required(VERSION 3.18)
project(adaptive_grading_android LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译优化：ARM64 NEON 向量化与快速数学
if(ANDROID)
    add_compile_options(-O3 -Wall -Wextra -ffast-math)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        add_compile_options(-march=armv8-a)
    endif()
endif()

# 包含路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../c_core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
)

# JNI 动态共享库 (编译目标为 libadaptive_grading_jni.so)
add_library(adaptive_grading_jni SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/../c_core/src/adaptive_grading.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c_core/src/adaptive_grading_neon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c_core/src/adaptive_thread_pool.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c_core/src/adaptive_logger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c_core/src/adaptive_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/jni/adaptive_grading_jni.c
)

# 链接系统库
find_library(log-lib log)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)

target_link_libraries(adaptive_grading_jni
    m
    ${log-lib}
    ${android-lib}
    ${jnigraphics-lib}
)

