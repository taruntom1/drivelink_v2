#include "core_os/periodic_timer.hpp"
#include "core_os/realtime.hpp"
#include "core_os/native_task.hpp"
#include <chrono>
namespace core_os {
PeriodicTimer::~PeriodicTimer() { stop(); }
Err PeriodicTimer::start(const char *, uint32_t period_us, OneShotFn fn, void *ctx) {
    if (m_state.thread.joinable() || !fn || !period_us) { return -1; }
    if (native::manualScheduling()) { return kOk; }
    m_state.stop = false;
    m_state.thread = std::thread([this, period_us, fn, ctx] {
        std::unique_lock lock(m_state.mutex);
        const auto period = std::chrono::microseconds(period_us);
        while (!m_state.cv.wait_for(lock, period, [this] { return m_state.stop; })) {
            lock.unlock();
            { RealtimeScope guard(true); fn(ctx); }
            lock.lock();
        }
    });
    return kOk;
}
void PeriodicTimer::stop() {
    { std::lock_guard lock(m_state.mutex); m_state.stop = true; }
    m_state.cv.notify_one();
    if (m_state.thread.joinable()) { m_state.thread.join(); }
}
}
