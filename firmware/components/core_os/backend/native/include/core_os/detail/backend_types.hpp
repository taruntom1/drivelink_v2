#pragma once

// Native backend state for the core_os types (std::thread, std::mutex,
// steady_clock). The native build (firmware/test/native) puts this directory
// on the include path; the IDF build uses its own copy of this header.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "core_os/types.hpp"

namespace core_os::detail {

struct MutexState {
    std::mutex mutex;
};

struct EventState {
    std::mutex mutex;
    std::condition_variable cv;
    bool set = false;
};

struct PeriodicTaskState {
    std::thread thread;
    std::mutex mutex; ///< guards stop and period_changed, for the condition variable
    std::condition_variable cv;
    bool stop = false;
    bool period_changed = false;
    std::atomic<bool> running{false};
    std::atomic<uint32_t> period_us{0};
    TickFn fn = nullptr;
    void *ctx = nullptr;
};

} // namespace core_os::detail
