#pragma once

#include <cstdint>

#include "core_os/detail/backend_types.hpp"

namespace core_os {

/// A binary signal from one task to one waiter, with static storage.
///
/// `signal()` sets it and never blocks, so a loop tick may call it. It does
/// not count: signals before a `wait()` collapse into one. `wait()` takes the
/// signal (clearing it) or times out. `clear()` drops a pending signal.
class Event {
public:
    Event();
    ~Event();

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    /// Set the event. Never blocks; safe from a loop tick.
    void signal();

    /// Wait for the event and clear it. Waits at least `timeout_us` before
    /// giving up (on the device it rounds up to whole FreeRTOS ticks, plus
    /// one), and 0 only polls. @return true if the event was set.
    bool wait(uint32_t timeout_us);

    /// Clear a pending signal. Never blocks.
    void clear();

private:
    detail::EventState m_state;
};

} // namespace core_os
