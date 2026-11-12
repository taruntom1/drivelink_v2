#include "diag.hpp"

#include <atomic>
#include "esp_timer.h"

#include "diag_internal.hpp"
#include "freertos/queue.h"

namespace diag {

namespace {

detail::SlotTable s_table;

StaticQueue_t s_queue_storage;
uint8_t s_queue_buffer[CONFIG_DIAG_RECORD_QUEUE_DEPTH * sizeof(detail::QueuedEvent)];
std::atomic<QueueHandle_t> s_queue{nullptr};



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

void report(Code code, esp_err_t err, uint32_t arg0, uint32_t arg1, uint8_t instance)
{
    const auto slot=s_table.resolve(code.slot,instance);
    if(slot>=detail::kRuntimeSlots) { s_table.add(codes::diag::kRecordDropped.slot,1);return; }
    s_table.raise(slot, code.clears);

    detail::QueuedEvent ev{};
    ev.timestamp_us = detail::wallClockUs();
    ev.esp_err = static_cast<int32_t>(err);
    ev.arg0 = arg0;
    ev.arg1 = arg1;
    ev.slot = static_cast<uint8_t>(slot);
    ev.kind = detail::QueuedKind::Record;
    ev.flags = detail::currentFlags();
    enqueue(ev);
}

void count(Code code, uint32_t n, uint8_t instance)
{
    const auto slot=s_table.resolve(code.slot,instance);
    if(slot>=detail::kRuntimeSlots) { s_table.add(codes::diag::kRecordDropped.slot,1);return; }
    s_table.add(slot, n);
}

void clear(Code code, uint8_t instance)
{
    const auto slot=s_table.resolve(code.slot,instance);
    if (!s_table.clearActive(slot)) {
        return;
    }
    detail::QueuedEvent ev{};
    ev.timestamp_us = detail::wallClockUs();
    ev.slot = static_cast<uint8_t>(slot);
    ev.kind = detail::QueuedKind::Cleared;
    ev.flags = detail::currentFlags();
    enqueue(ev);
}

uint8_t activeErrorCount() {
    uint32_t n=0;
    for(size_t i=0;i<detail::kRuntimeSlots;++i) {
        if(s_table.allocated(i) && (s_table.read(i).state & protocol::kStateActive) &&
            kCodeTable[s_table.codeSlot(i)].severity==Severity::Error) { ++n; }
    }
    return static_cast<uint8_t>(n);
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
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint8_t currentFlags()
{
    return 0U;
}

} // namespace detail

} // namespace diag
