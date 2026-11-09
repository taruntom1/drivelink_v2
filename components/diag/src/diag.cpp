#include "diag.hpp"

#include <atomic>
#include <sys/time.h>

#include "diag_internal.hpp"
#include "freertos/queue.h"

namespace diag {

namespace {

detail::SlotTable s_table;

StaticQueue_t s_queue_storage;
uint8_t s_queue_buffer[CONFIG_DIAG_RECORD_QUEUE_DEPTH * sizeof(detail::QueuedEvent)];
std::atomic<QueueHandle_t> s_queue{nullptr};

std::atomic<bool> s_time_synced{false};

void enqueue(const detail::QueuedEvent &ev)
{
    QueueHandle_t q = s_queue.load(std::memory_order_acquire);
    if (q == nullptr || xQueueSend(q, &ev, 0) != pdTRUE) {
        s_table.add(codes::diag::kRecordDropped.slot, 1U);
    }
}

} // namespace

void init()
{
    // Function-local static: created exactly once, even with concurrent callers.
    static QueueHandle_t queue = xQueueCreateStatic(CONFIG_DIAG_RECORD_QUEUE_DEPTH,
                                                    sizeof(detail::QueuedEvent), s_queue_buffer,
                                                    &s_queue_storage);
    s_queue.store(queue, std::memory_order_release);
}

void report(Code code, esp_err_t err, uint32_t arg0, uint32_t arg1)
{
    s_table.raise(code.slot, code.clears);

    detail::QueuedEvent ev{};
    ev.timestamp_us = detail::wallClockUs();
    ev.esp_err = static_cast<int32_t>(err);
    ev.arg0 = arg0;
    ev.arg1 = arg1;
    ev.slot = code.slot;
    ev.kind = detail::QueuedKind::Record;
    ev.flags = detail::currentFlags();
    enqueue(ev);
}

void count(Code code, uint32_t n)
{
    s_table.add(code.slot, n);
}

void clear(Code code)
{
    if (!s_table.clearActive(code.slot)) {
        return;
    }
    detail::QueuedEvent ev{};
    ev.timestamp_us = detail::wallClockUs();
    ev.slot = code.slot;
    ev.kind = detail::QueuedKind::Cleared;
    ev.flags = detail::currentFlags();
    enqueue(ev);
}

void setTimeSynced()
{
    s_time_synced.store(true, std::memory_order_relaxed);
}

bool isTimeSynced()
{
    return s_time_synced.load(std::memory_order_relaxed);
}

namespace detail {

SlotTable &table()
{
    return s_table;
}

bool peekEvent(QueuedEvent &out)
{
    QueueHandle_t q = s_queue.load(std::memory_order_acquire);
    return q != nullptr && xQueuePeek(q, &out, 0) == pdTRUE;
}

void popEvent()
{
    QueueHandle_t q = s_queue.load(std::memory_order_acquire);
    if (q != nullptr) {
        QueuedEvent discard{};
        (void)xQueueReceive(q, &discard, 0);
    }
}

uint64_t wallClockUs()
{
    timeval now{};
    gettimeofday(&now, nullptr);
    return (static_cast<uint64_t>(now.tv_sec) * 1000000ULL) + static_cast<uint64_t>(now.tv_usec);
}

uint8_t currentFlags()
{
    return isTimeSynced() ? protocol::kFlagTimeSynced : 0U;
}

} // namespace detail

} // namespace diag
