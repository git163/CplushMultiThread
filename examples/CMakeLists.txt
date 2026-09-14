# examples/CMakeLists.txt — 场景示例
# 每个 .cpp 编译成一个可独立运行的程序；新增示例只需追加一条 add_example(...)。

function(add_example name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
    target_link_libraries(${name} PRIVATE task_runner)
endfunction()

add_example(sync_task)
add_example(async_task)
add_example(progress_and_finish)
add_example(multi_task)
add_example(reusable_components)
