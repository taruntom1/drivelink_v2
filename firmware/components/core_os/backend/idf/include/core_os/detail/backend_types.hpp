#pragma once

// ESP-IDF backend state for the core_os types. The IDF build puts this
// directory on the include path; the native build uses its own copy of this
// header. Only core_os's API headers include it.

#include <atomic>
#include <cstdint>

#include "core_os/types.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "stoppable_task.hpp"
#include "task_timer.hpp"

namespace core_os::detail {

struct MutexState {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
};

struct EventState {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
};

struct PeriodicTaskState {
    rtos_util::StoppableTask task;
    rtos_util::TaskTimer timer;
    TickFn fn = nullptr;
    void *ctx = nullptr;
    std::atomic<uint32_t> period_us{0};
};

} // namespace core_os::detail
