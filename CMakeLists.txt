cmake_minimum_required(VERSION 3.20)
project(CplushMultiThread LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认使用 RelWithDebInfo：优化 + 保留调试符号
# - 性能接近 Release
# - 崩溃时仍能用 gdb/lldb 拿到符号和行号
# - 真正发布时单独切到 Release 即可（-DCMAKE_BUILD_TYPE=Release）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 调试信息：所有 build type 都保留 -g，便于事后排查
add_compile_options(-g)

# 源码与头文件
# 注意：file(GLOB_RECURSE) 是 CMake 反模式——CMake 不会主动感知新增的 .cpp。
# 缓解：CONFIGURE_DEPENDS 会在 build 时触发 cmake 重跑；如 build 已存在，
#      新增 .cpp 后请先 `cmake --build build --target rebuild_cache`。
# 项目成长后推荐：每个模块一个 CMakeLists.txt，用 add_subdirectory() 替代 GLOB。
file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.cc")
file(GLOB_RECURSE PROJECT_HEADERS CONFIGURE_DEPENDS "src/include/*.h" "src/include/*.hpp")

# 主目标
find_package(Threads REQUIRED)

# task_runner：仅头文件框架（src/include/task_runner/）。
# 以 INTERFACE 库暴露 include 目录与 pthread 链接，主程序与测试共用。
add_library(task_runner INTERFACE)
target_include_directories(task_runner INTERFACE src/include)
target_link_libraries(task_runner INTERFACE Threads::Threads)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src src/include)
target_link_libraries(${PROJECT_NAME} PRIVATE task_runner)

# 启用测试
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()