// core_os, ESP-IDF backend: the clock, Mutex, Event and the diag sink.

#include <cinttypes>

#include "core_os/capture_memory.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/event.hpp"
#include "core_os/memory.hpp"
#include "core_os/mutex.hpp"
#include "diag.hpp"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tick_util.hpp"

static const char *const TAG = "core_os";

namespace core_os {

int64_t nowUs()
{
    return esp_timer_get_time();
}

// -- Capture memory --------------------------------------------------------------
// PSRAM is caps-alloc only (CONFIG_SPIRAM_USE_CAPS_ALLOC): nothing lands there
// unless asked. A board without PSRAM (CONFIG_SPIRAM_IGNORE_NOTFOUND) has no
// SPIRAM heap and falls back to a small internal ring.

namespace {
bool hasPsram()
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized() && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return false; // esp_psram has no driver without CONFIG_SPIRAM
#endif
}
} // namespace

size_t captureCapacity(size_t limit_bytes)
{
    const size_t available = hasPsram() ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                        : kCaptureFallbackBytes;
    return available < limit_bytes ? available : limit_bytes;
}

bool captureUsesInternalRam()
{
    return !hasPsram();
}

size_t freeInternalBytes()
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *captureAlloc(size_t bytes)
{
    const uint32_t caps = hasPsram() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return heap_caps_malloc(bytes, caps);
}

void captureFree(void *ring)
{
    heap_caps_free(ring);
}

// -- Mutex ---------------------------------------------------------------------

Mutex::Mutex() : m_state{}
{
    // Static storage: cannot fail.
    m_state.handle = xSemaphoreCreateMutexStatic(&m_state.storage);
}

Mutex::~Mutex()
{
    vSemaphoreDelete(m_state.handle);
}

void Mutex::lock()
{
    (void)xSemaphoreTake(m_state.handle, portMAX_DELAY);
}

bool Mutex::tryLock()
{
    return xSemaphoreTake(m_state.handle, 0) == pdTRUE;
}

void Mutex::unlock()
{
    (void)xSemaphoreGive(m_state.handle);
}

// -- Event ---------------------------------------------------------------------

Event::Event() : m_state{}
{
    m_state.handle = xSemaphoreCreateBinaryStatic(&m_state.storage);
}

Event::~Event()
{
    vSemaphoreDelete(m_state.handle);
}

void Event::signal()
{
    // Giving a binary semaphore that is already given fails harmlessly.
    (void)xSemaphoreGive(m_state.handle);
}

bool Event::wait(uint32_t timeout_us)
{
    TickType_t ticks = 0;
    if (timeout_us > 0U) {
        // Round up to ticks, plus one: a wait of n ticks can end up to one
        // tick early, and the timeout is a minimum.
        const auto ms = static_cast<uint32_t>((static_cast<uint64_t>(timeout_us) + 999U) / 1000U);
        ticks = rtos_util::msToTicksCeil(ms) + 1U;
    }
    return xSemaphoreTake(m_state.handle, ticks) == pdTRUE;
}

void Event::clear()
{
    (void)xSemaphoreTake(m_state.handle, 0);
}

// -- diag sink -----------------------------------------------------------------

void reportDiag(DiagCode code, Err err, uint32_t arg0, uint32_t arg1, uint8_t instance)
{
    // The core is pure and has no log text of its own: write the code and the
    // args, which carry the detail (see protocol/defs/diag_codes.yaml).
    if (code.severity == diag::Severity::Error) {
        ESP_LOGE(TAG, "diag source 0x%02x code 0x%04x: err %s, arg0 0x%" PRIx32 ", arg1 0x%" PRIx32,
                 static_cast<unsigned>(code.source), static_cast<unsigned>(code.code),
                 esp_err_to_name(static_cast<esp_err_t>(err)), arg0, arg1);
    } else {
        ESP_LOGW(TAG, "diag source 0x%02x code 0x%04x: err %s, arg0 0x%" PRIx32 ", arg1 0x%" PRIx32,
                 static_cast<unsigned>(code.source), static_cast<unsigned>(code.code),
                 esp_err_to_name(static_cast<esp_err_t>(err)), arg0, arg1);
    }
    diag::report(code, static_cast<esp_err_t>(err), arg0, arg1, instance);
}

void countDiag(DiagCode code, uint32_t n, uint8_t instance)
{
    diag::count(code, n, instance);
}
void raiseDiag(DiagCode code, uint8_t instance) { diag::raise(code, instance); }
void clearDiag(DiagCode code, uint8_t instance) { diag::clear(code, instance); }
void setDiagTiming(DiagTiming which, uint32_t value)
{
    switch (which) {
        case DiagTiming::CounterPeriodMs: diag::setCounterPeriodMs(value); break;
        case DiagTiming::PollMs: diag::setPollMs(value); break;
        case DiagTiming::StackLogS: diag::setStackLogPeriodS(value); break;
    }
}
bool diagBlocksArm() { return diag::blocksArm(); }
bool diagDisarmsActive() { return diag::disarmsActive(); }
void setDiagDisarmHandler(void *ctx, DiagDisarmFn fn) { diag::setDisarmHandler(ctx, fn); }

} // namespace core_os
