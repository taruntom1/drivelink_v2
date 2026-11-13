#pragma once

// Native backend state for the core_os types (std::thread, std::mutex,
// steady_clock). The native build (firmware/test/native) puts this directory
// on the include path; the IDF build uses its own copy of this header.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

/// The in-memory KvStore with its fault injection (core_os/native_kv.hpp).
struct KvStoreState {
    std::string directory; ///< empty for in-memory tests
    std::map<std::string, std::map<std::string, std::vector<uint8_t>>> data;
    uint32_t write_count = 0;      ///< writes (set and erase) that took effect
    int64_t power_loss_after = -1; ///< writes left before every write fails; -1 never
    int64_t fail_after = -1;       ///< writes left before one write fails; -1 never
    Err fail_err = 0;
    int64_t tear_after = -1;       ///< writes left before one set is torn; -1 never
    uint8_t tear_mode = 0;         ///< native::Tear
};

struct OneShotState {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool cancel = false;
};

struct PeriodicTimerState {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
};

} // namespace core_os::detail
