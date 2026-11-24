#pragma once

// Native backend only: what the diag sink recorded, for the tests.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core_os/diag_sink.hpp"
namespace diag::detail { class SlotTable; }

namespace core_os::native {

struct RecordedReport {
    DiagCode code;
    Err err;
    uint32_t arg0;
    uint32_t arg1;
    uint8_t instance = 255;
};

using ReportSink = void (*)(void *, const RecordedReport &);
using CountSink = void (*)(void *, DiagCode, uint32_t, uint8_t);
// Set before starting tasks, clear after joining them. Null restores the
// recording test backend. The vdev supplies bounded, fixed-storage sinks.
void setDiagSinks(void *ctx, ReportSink report, CountSink count);

/// Every `reportDiag` call since the last reset, in order.
std::vector<RecordedReport> recordedReports();

/// How many times `code` was reported.
size_t reportCount(DiagCode code);

/// The total `countDiag` adds to `code`.
uint64_t counterTotal(DiagCode code, uint8_t instance = 255);

/// Forget everything recorded.
void resetRecordedDiag();
// Same atomic state used by the native safety interlock and vdev reporter.
diag::detail::SlotTable &diagTable();
/// The last `setDiagTiming` value (0 before the first).
uint32_t diagTiming(DiagTiming which);

} // namespace core_os::native
