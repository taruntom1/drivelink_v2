// core_os, native backend: the clock, Mutex, Event, PeriodicTask and a
// recording diag sink. Built by firmware/test/native (and later the vdev).

#include <chrono>
#include <cstdlib>
#include <map>
#include <utility>
#include <tuple>
#include <atomic>
#include <array>
#include "core_os/native_task.hpp"

#include "core_os/capture_memory.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/event.hpp"
#include "core_os/mutex.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/periodic_task.hpp"
#include "core_os/realtime.hpp"
#include "diag_table.hpp"
#include "diag_observer.hpp"

namespace core_os {

namespace {

constexpr Err kNativeError = -1;

using Clock = std::chrono::steady_clock;

const Clock::time_point s_epoch = Clock::now();
std::atomic<int64_t> s_boot_offset{0};
std::atomic<int32_t> s_start_after{-1};
std::atomic<int32_t> s_period_after{-1};
std::atomic<bool> s_manual_scheduling{false};
native::PeriodicStartObserver s_periodic_start_observer = nullptr;
std::atomic<size_t> s_capture_capacity{4U * 1024U * 1024U};
std::atomic<bool> s_capture_fail_next{false};

} // namespace

// The native device has 4 MiB of "PSRAM" unless a test says otherwise.
size_t captureCapacity(size_t limit_bytes)
{
    const size_t available = s_capture_capacity.load();
    return available < limit_bytes ? available : limit_bytes;
}
void *captureAlloc(size_t bytes)
{
    if (s_capture_fail_next.exchange(false)) { return nullptr; }
    return std::malloc(bytes ? bytes : 1);
}
void captureFree(void *ring) { std::free(ring); }
namespace native {
void setCaptureCapacity(size_t bytes) { s_capture_capacity.store(bytes); }
void failNextCaptureAlloc() { s_capture_fail_next.store(true); }
}

int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s_epoch).count()-s_boot_offset.load();
}
namespace native {
void setManualScheduling(bool manual) { s_manual_scheduling.store(manual); }
PeriodicStartObserver setPeriodicStartObserver(PeriodicStartObserver observer)
{
    const auto previous = s_periodic_start_observer;
    s_periodic_start_observer = observer;
    return previous;
}
bool manualScheduling() { return s_manual_scheduling.load(); }
void failPeriodicStartAfter(int32_t successes) { s_start_after.store(successes); }
void failNextPeriodChange() { s_period_after.store(0); }
void failPeriodChangeAfter(int32_t successes) { s_period_after.store(successes); }
void resetBootClock() { s_boot_offset.store(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-s_epoch).count()); }
}

// -- Mutex ---------------------------------------------------------------------

Mutex::Mutex() = default;
Mutex::~Mutex() = default;

void Mutex::lock()
{
    recordRealtimeViolation();
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
    recordRealtimeViolation();
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
                        TickFn fn, void *ctx, bool realtime)
{
    (void)realtime;
    if (s_start_after.load() == 0) { s_start_after.store(-1); return kNativeError; }
    if (s_start_after.load() > 0) { --s_start_after; }
    (void)name;
    (void)stack_bytes;
    (void)priority;
    (void)core;
    if (m_state.running.load() || fn == nullptr || period_us == 0U) {
        return kNativeError;
    }
    if (s_periodic_start_observer) { s_periodic_start_observer(name, core); }
    m_state.fn = fn;
    m_state.ctx = ctx;
    m_state.period_us.store(period_us);
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        m_state.stop = false;
        m_state.period_changed = false;
    }
    m_state.running.store(true);
    if (native::manualScheduling()) { return kOk; }

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
    if (s_period_after.load() == 0) { s_period_after.store(-1); return kNativeError; }
    if (s_period_after.load() > 0) { --s_period_after; }
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
    struct Counter { std::atomic<uint32_t> identity{0}; std::atomic<uint64_t> value{0}; };
    std::array<Counter, 2048> counters{};
};
void *s_diag_ctx=nullptr;
native::ReportSink s_report_sink=nullptr;
native::CountSink s_count_sink=nullptr;
diag::detail::SlotTable s_diag_table;
diag::detail::DisarmObserver s_diag_observer;

DiagRecord &diagRecord()
{
    static DiagRecord record;
    return record;
}

uint32_t key(DiagCode code, uint8_t instance = 255)
{
    return (static_cast<uint32_t>(code.source) << 24U) | (static_cast<uint32_t>(code.code) << 8U) | instance;
}

} // namespace

void reportDiag(DiagCode code, Err err, uint32_t arg0, uint32_t arg1, uint8_t instance)
{
    const auto slot = s_diag_table.resolve(code.slot, instance);
    if (slot >= diag::detail::kRuntimeSlots) { s_diag_table.safetyOverflow(code,instance); }
    s_diag_table.raise(slot, code.clears);
    s_diag_observer.notify(code);
    if(s_report_sink) { s_report_sink(s_diag_ctx,{code,err,arg0,arg1,instance});return; }
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.reports.push_back({code, err, arg0, arg1, instance});
}

void countDiag(DiagCode code, uint32_t n, uint8_t instance)
{
    if (!n) { return; }
    s_diag_table.add(s_diag_table.resolve(code.slot, instance), n);
    s_diag_observer.notify(code);
    if(s_count_sink) { s_count_sink(s_diag_ctx,code,n,instance);return; }
    DiagRecord &r = diagRecord();
    const uint32_t identity = key(code, instance);
    for (auto &counter : r.counters) {
        uint32_t expected = 0;
        if (counter.identity.load(std::memory_order_relaxed) == identity ||
            counter.identity.compare_exchange_strong(expected, identity, std::memory_order_relaxed) || expected == identity) {
            counter.value.fetch_add(n, std::memory_order_relaxed); return;
        }
    }
}

void raiseDiag(DiagCode code, uint8_t instance) {
    const auto slot = s_diag_table.resolve(code.slot, instance);
    if (slot >= diag::detail::kRuntimeSlots) { s_diag_table.safetyOverflow(code,instance); }
    if (s_diag_table.latch(slot)) { s_diag_table.markDirty(slot); }
    s_diag_observer.notify(code);
}
void clearDiag(DiagCode code, uint8_t instance) {
    const auto slot = s_diag_table.resolve(code.slot, instance);
    if (slot >= diag::detail::kRuntimeSlots) { s_diag_table.safetyOverflow(code,instance,false); return; }
    if (s_diag_table.clearActive(slot)) { s_diag_table.markDirty(slot, 2); }
}
bool diagBlocksArm() { return s_diag_table.activeSafety(false); }
bool diagDisarmsActive() { return s_diag_table.activeSafety(true); }
void setDiagDisarmHandler(void *ctx, DiagDisarmFn fn) {
    s_diag_observer.disable();
    while (!s_diag_observer.idle()) { std::this_thread::yield(); }
    if (fn) { s_diag_observer.install(ctx, fn); }
}

namespace native {

diag::detail::SlotTable &diagTable() { return s_diag_table; }

void setDiagSinks(void *ctx,ReportSink report,CountSink count) { s_diag_ctx=ctx;s_report_sink=report;s_count_sink=count; }

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

uint64_t counterTotal(DiagCode code, uint8_t instance)
{
    DiagRecord &r = diagRecord();
    for (const auto &counter : r.counters) {
        if (counter.identity.load() == key(code, instance)) { return counter.value.load(); }
    }
    return 0;
}

void resetRecordedDiag()
{
    DiagRecord &r = diagRecord();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.reports.clear();
    for (auto &counter : r.counters) { counter.value.store(0); counter.identity.store(0); }
    s_diag_table.clearOccurred(); s_diag_table.resetCounts();
    s_diag_table.resetSafetyOverflow();
    for (size_t slot = 0; slot < diag::detail::kRuntimeSlots; ++slot) {
        s_diag_table.clearActive(slot); (void)s_diag_table.takeDirty(slot);
    }
}

} // namespace native

} // namespace core_os
