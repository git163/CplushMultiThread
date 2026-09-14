#ifndef EXAMPLES_DEMO_UTIL_HPP_
#define EXAMPLES_DEMO_UTIL_HPP_

// examples：示例共用的小工具（仅供 Demo 打印，不属于库本身）。

#include "task_runner/runnable_task.hpp"

namespace demo {

inline const char* to_str(task_runner::error_code e) {
    switch (e) {
        case task_runner::error_code::ok:               return "ok";
        case task_runner::error_code::already_running:  return "already_running";
        case task_runner::error_code::already_stopping: return "already_stopping";
        case task_runner::error_code::not_running:      return "not_running";
        case task_runner::error_code::self_stop_denied: return "self_stop_denied";
        case task_runner::error_code::resource_error:   return "resource_error";
    }
    return "?";
}

inline const char* to_str(task_runner::run_result r) {
    switch (r) {
        case task_runner::run_result::none:      return "none";
        case task_runner::run_result::completed: return "completed";
        case task_runner::run_result::stopped:   return "stopped";
        case task_runner::run_result::failed:    return "failed";
    }
    return "?";
}

}  // namespace demo

#endif  // EXAMPLES_DEMO_UTIL_HPP_
