#pragma once

/// diag: report errors and warnings to the host as codes over the control
/// UART, and (optionally) as text on the console.
///
/// Codes are defined in diag_codes.yaml and generated into diag_codes.hpp
/// (`diag::codes::<source>::k<Name>`). Use them only through the macros:
///
///   DIAG_ERROR(code, esp_err, arg0, arg1, "fmt", ...)   task context only
///   DIAG_WARN (code, esp_err, arg0, arg1, "fmt", ...)   task context only
///   DIAG_COUNT(code) / DIAG_COUNT_N(code, n)            any context, real-time safe
///   DIAG_RAISE(code)                                    latched, real-time safe
///   DIAG_CLEAR(code)                                    latched: real-time safe; records: task only
///
/// The macros check the code's kind and severity at compile time, so the
/// code must be a constant. DIAG_ERROR / DIAG_WARN log with the file's `TAG`.
/// With CONFIG_DIAG_CONSOLE_LOG off, the text part compiles to nothing: no
/// format string in the binary, and the log arguments are not evaluated.
///
/// Sending happens on the diag::Reporter task (diag_reporter.hpp). Until it
/// runs, records wait in the record queue.

#include <cstdint>

#include "diag_codes.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace diag {

/// Set up the record queue (static storage, cannot fail). Call it first in
/// app_main, before anything can report. Idempotent and thread-safe.
/// A record reported before init() is counted as RecordDropped.
void init();

/// Latch a record and queue it for the host. Use DIAG_ERROR / DIAG_WARN.
/// Task context only: it reads the wall clock and posts to a queue (no
/// waiting). Not for ISRs, critical sections, or while holding m_mutex.
void report(Code code, esp_err_t err, uint32_t arg0, uint32_t arg1, uint8_t instance = 255);

/// Add `n` to a counter code. Use DIAG_COUNT / DIAG_COUNT_N.
/// Real-time safe: two relaxed atomic operations, from any context.
void count(Code code, uint32_t n, uint8_t instance = 255);
void raise(Code code, uint8_t instance = 255);
bool blocksArm();
bool disarmsActive();
using DisarmFn = void (*)(void *, Code);
void setDisarmHandler(void *ctx, DisarmFn fn);

/// Clear a code's active bit and, if it was set, queue a Cleared event.
/// Use DIAG_CLEAR. Latched codes use atomic dirty bits; record codes are task-only.
void clear(Code code, uint8_t instance = 255);
uint8_t activeErrorCount();

/// The live timing settings (`diag.*` system params). The core sets them
/// through core_os; the reporter and main's stack log read them. Any task:
/// word-sized atomics. Until the core boots they are the params' defaults.
void setCounterPeriodMs(uint32_t ms);
void setPollMs(uint32_t ms);
void setStackLogPeriodS(uint32_t s); ///< 0: off
uint32_t counterPeriodMs();
uint32_t pollMs();
uint32_t stackLogPeriodS();

namespace detail {

/// Type-checks a log format string and its arguments; never called.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline void checkLogFormat(const char *fmt, ...)
{
    (void)fmt;
}

} // namespace detail

} // namespace diag

#define DIAG_RAISE(code) do { static_assert((code).kind == ::diag::CodeKind::Latched); ::diag::raise(code); } while (false)
#define DIAG_RAISE_INSTANCE(code, instance) do { static_assert((code).kind == ::diag::CodeKind::Latched); ::diag::raise(code, instance); } while (false)

#if CONFIG_DIAG_CONSOLE_LOG
#define DIAG_DETAIL_LOG_ERROR(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define DIAG_DETAIL_LOG_WARN(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
// `if (false)`: the format is still checked and the arguments still count as
// used (no unused-variable warnings), but nothing is evaluated or emitted.
#define DIAG_DETAIL_LOG_OFF(fmt, ...)                                                                \
    do {                                                                                             \
        if (false) {                                                                                 \
            (void)TAG;                                                                               \
            ::diag::detail::checkLogFormat(fmt, ##__VA_ARGS__);                                      \
        }                                                                                            \
    } while (0)
#define DIAG_DETAIL_LOG_ERROR(fmt, ...) DIAG_DETAIL_LOG_OFF(fmt, ##__VA_ARGS__)
#define DIAG_DETAIL_LOG_WARN(fmt, ...) DIAG_DETAIL_LOG_OFF(fmt, ##__VA_ARGS__)
#endif

/// Report an error-severity record code: console line + queued record.
#define DIAG_ERROR(code, err, arg0, arg1, fmt, ...)                                                  \
    do {                                                                                             \
        static_assert((code).kind == ::diag::CodeKind::Record &&                                    \
                          (code).severity == ::diag::Severity::Error,                               \
                      "DIAG_ERROR needs an error-severity record code");                             \
        DIAG_DETAIL_LOG_ERROR(fmt, ##__VA_ARGS__);                                                   \
        ::diag::report((code), (err), static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1));    \
    } while (0)

/// Report a warning-severity record code: console line + queued record.
#define DIAG_WARN(code, err, arg0, arg1, fmt, ...)                                                   \
    do {                                                                                             \
        static_assert((code).kind == ::diag::CodeKind::Record &&                                    \
                          (code).severity == ::diag::Severity::Warning,                             \
                      "DIAG_WARN needs a warning-severity record code");                             \
        DIAG_DETAIL_LOG_WARN(fmt, ##__VA_ARGS__);                                                    \
        ::diag::report((code), (err), static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1));    \
    } while (0)

/// Count one occurrence of a counter code. Real-time safe; no log.
#define DIAG_COUNT(code) DIAG_COUNT_N(code, 1U)

/// Count `n` occurrences of a counter code. Real-time safe; no log.
#define DIAG_COUNT_N(code, n)                                                                        \
    do {                                                                                             \
        static_assert((code).kind == ::diag::CodeKind::Counter, "DIAG_COUNT needs a counter code"); \
        ::diag::count((code), static_cast<uint32_t>(n));                                             \
    } while (0)

/// The condition behind a `clears: true` code is gone.
#define DIAG_CLEAR(code)                                                                             \
    do {                                                                                             \
        static_assert((code).kind != ::diag::CodeKind::Counter && (code).clears,                     \
                      "DIAG_CLEAR needs a record code with clears: true");                           \
        ::diag::clear((code));                                                                       \
    } while (0)
