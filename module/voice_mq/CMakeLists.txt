# ------------------------------------------------------------------
# module/voice_mq — 语音识别文本流转模拟程序（C++17 / x86 Ubuntu）
#
# 依赖：仅 POSIX 消息队列（libc 的 mq_*）与 pthread（-lrt 兼容旧 glibc），
#       无任何 RK 硬件库。
#
# 编译：
#   cmake -B build && cmake --build build -j
# 运行：
#   ./build/voice_mq_demo          # 直到 Ctrl+C
#   ./build/voice_mq_demo 5        # 运行 5 秒
# ------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)
project(voice_mq_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

add_executable(voice_mq_demo
  src/main.cpp
  src/VoiceMqSim.cpp
)

target_include_directories(voice_mq_demo PRIVATE include)

target_link_libraries(voice_mq_demo PRIVATE Threads::Threads)

# POSIX 消息队列：glibc < 2.34 需要 -lrt（新 glibc 已并入 libc，无副作用）
target_link_libraries(voice_mq_demo PRIVATE rt)
