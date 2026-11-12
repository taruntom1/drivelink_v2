// core_os, native backend: the clock, Mutex, Event, PeriodicTask and a
// recording diag sink. Built by firmware/test/native (and later the vdev).

#include <chrono>
#include <map>
#include <utility>

#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/event.hpp"
#include "core_os/mutex.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/periodic_task.hpp"

namespace core_os {

namespace {

constexpr Err kNativeError = -1;

using Clock = std::chrono::steady_clock;

const Clock::time_point s_epoch = Clock::now();

} // namespace

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s_epoch).count();
}

// -- Mutex ---------------------------------------------------------------------

Mutex::Mutex() = default;
Mutex::~Mutex() = default;

void Mutex::lock()
{
    m_state.mutex.lock();
}

bool Mutex::tryLock()
{
    return m_state.mutex.try_lock();
}

void Mutex::unlock()
{
    m_state.mutex.unlock();
}

// -- Event ---------------------------------------------------------------------

Event::Event() = default;
Event::~Event() = default;

void Event::signal()
{
    // Natively this takes a std::mutex for a few instructions. It is a test
    // backend; the device's signal is a non-blocking semaphore give.
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.set = true;
    }
    m_state.cv.notify_one();
}

bool Event::wait(uint32_t timeout_us)
{
    std::unique_lock<std::mutex> lock(m_state.mutex);
    if (!m_state.set && timeout_us > 0U) {
        (void)m_state.cv.wait_for(lock, std::chrono::microseconds(timeout_us), [this] { return m_state.set; });
    }
    const bool was_set = m_state.set;
    m_state.set = false;
    return was_set;
}

void Event::clear()
{
    std::lock_guard<std::mutex> lock(m_state.mutex);
    m_state.set = false;
}

// -- PeriodicTask --------------------------------------------------------------

PeriodicTask::PeriodicTask() = default;

PeriodicTask::~PeriodicTask()
{
    stop();
}

Err PeriodicTask::start(const char *name, uint32_t stack_bytes, uint8_t priority, int8_t core, uint32_t period_us,
                        TickFn fn, void *ctx)
{
    (void)name;
    (void)stack_bytes;
    (void)priority;
    (void)core;
    if (m_state.running.load() || fn == nullptr || period_us == 0U) {
        return kNativeError;
    }
    m_state.fn = fn;
    m_state.ctx = ctx;
    m_state.period_us.store(period_us);
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.stop = false;
        m_state.period_changed = false;
    }
    m_state.running.store(true);

    detail::PeriodicTaskState *state = &m_state;
    m_state.thread = std::thread([state]() {
        auto period = std::chrono::microseconds(state->period_us.load());
        Clock::time_point next = Clock::now() + period;
        std::unique_lock<std::mutex> lock(state->mutex);
        while (!state->stop) {
            state->cv.wait_until(lock, next, [state] { return state->stop || state->period_changed; });
            if (state->stop) {
                break;
            }
            if (state->period_changed) {
                // Restart the timer at the new period, as esp_timer_restart does.
                state->period_changed = false;
                period = std::chrono::microseconds(state->period_us.load());
                next = Clock::now() + period;
                continue;
            }
            const Clock::time_point now = Clock::now();
            if (now < next) {
                continue; // spurious wake-up
            }
            // Catch-up count: every deadline that went by since the last call.
            const auto late = now - next;
            const uint32_t elapsed = 1U + static_cast<uint32_t>(late / period);
            next += period * elapsed;
            lock.unlock();
            state->fn(state->ctx, elapsed);
            lock.lock();
        }
    });
    return kOk;
}

Err PeriodicTask::setPeriod(uint32_t period_us)
{
    if (period_us == 0U) {
        return kNativeError;
    }
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.period_us.store(period_us);
        m_state.period_changed = true;
    }
    m_state.cv.notify_one();
    return kOk;
}

void PeriodicTask::stop()
{
    if (!m_state.running.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.stop = true;
    }
    m_state.cv.notify_one();
    if (m_state.thread.joinable()) {
        m_state.thread.join();
    }
    m_state.running.store(false);
}

bool PeriodicTask::isRunning() const
{
    return m_state.running.load();
}

uint32_t PeriodicTask::periodUs() const
{
    return m_state.period_us.load();
}

uint32_t PeriodicTask::stackHighWaterMark() const
{
    return 0U;
}

// -- diag sink -----------------------------------------------------------------

namespace {

struct DiagRecord {
    std::mutex mutex;
    std::vector<native::RecordedReport> reports;
    std::map<std::pair<uint8_t, uint16_t>, uint64_t> counters;
};

DiagRecord &diagRecord()
{
    static DiagRecord record;
    return record;
}

std::pair<uint8_t, uint16_t> key(DiagCode code)
{
    return {static_cast<uint8_t>(code.source), code.code};
}

} // namespace

void reportDiag(DiagCode code, Err err, uint32_t arg0, uint32_t arg1, uint8_t instance)
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.reports.push_back({code, err, arg0, arg1, instance});
}

void countDiag(DiagCode code, uint32_t n)
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.counters[key(code)] += n;
}

namespace native {

std::vector<RecordedReport> recordedReports()
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.reports;
}

size_t reportCount(DiagCode code)
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    size_t n = 0;
    for (const RecordedReport &rep : r.reports) {
        if (key(rep.code) == key(code)) {
            ++n;
        }
    }
    return n;
}

uint64_t counterTotal(DiagCode code)
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.counters.find(key(code));
    return (it == r.counters.end()) ? 0U : it->second;
}

void resetRecordedDiag()
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.reports.clear();
    r.counters.clear();
}

} // namespace native

} // namespace core_os
