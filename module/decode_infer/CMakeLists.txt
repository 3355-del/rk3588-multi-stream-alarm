# ------------------------------------------------------------------
# module/decode_infer — 多路RTSP视频解析 + AI推理业务模块（x86 mock 版）
#
# 依赖：复用根工程的公共组件（Logger / SimScene / RingBuffer / Types），
#       不重复实现；仅新增解码+推理业务抽象与 mock。
#
# 编译（在模块目录内）：
#   cmake -B build && cmake --build build -j
#   或由根 CMakeLists 通过 add_subdirectory(module/decode_infer) 引入
# ------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)
project(decode_infer_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wshadow)
endif()

find_package(Threads REQUIRED)

# 根工程公共源文件（相对模块目录：../../）
set(ROOT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# 本模块核心库（解码+推理，不含 main，便于复用）
add_library(decode_infer_core STATIC
  src/MockDecoder.cpp
  src/MockAiInfer.cpp
  src/MultiStreamEngine.cpp
  ${ROOT_SRC}/src/common/Logger.cpp
  ${ROOT_SRC}/src/hal/SimScene.cpp
)

target_include_directories(decode_infer_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include   # 模块头（IDecoder/IAiInfer/...）
  ${ROOT_SRC}/include                   # 根工程公共头（msad/*）
)

target_link_libraries(decode_infer_core PUBLIC Threads::Threads)

# 测试示例：2 路模拟视频源
add_executable(decode_infer_demo src/main.cpp)
target_link_libraries(decode_infer_demo PRIVATE decode_infer_core Threads::Threads)
