#pragma once

// Native backend only: what the diag sink recorded, for the tests.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core_os/diag_sink.hpp"

namespace core_os::native {

struct RecordedReport {
    DiagCode code;
    Err err;
    uint32_t arg0;
    uint32_t arg1;
};

/// Every `reportDiag` call since the last reset, in order.
std::vector<RecordedReport> recordedReports();

/// How many times `code` was reported.
size_t reportCount(DiagCode code);

/// The total `countDiag` adds to `code`.
uint64_t counterTotal(DiagCode code);

/// Forget everything recorded.
void resetRecordedDiag();

} // namespace core_os::native
