#pragma once

#include <cstdint>

#include "core_os/types.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

namespace core_os {

using DiagCode = drivelink::protocol::defs::diag::Code;

/// Report a record code, as `DIAG_ERROR` / `DIAG_WARN` would. On the device
/// it calls `diag::report` and writes a console line; natively it records the
/// call for the tests (`core_os/native_diag.hpp`). Task context only.
void reportDiag(DiagCode code, Err err, uint32_t arg0, uint32_t arg1, uint8_t instance = 255);

/// Add `n` to a counter code, as `DIAG_COUNT_N` would. Real-time safe on the
/// device.
void countDiag(DiagCode code, uint32_t n);

} // namespace core_os
