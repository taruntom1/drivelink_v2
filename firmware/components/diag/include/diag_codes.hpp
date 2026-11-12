#pragma once

// The diag code table is generated from protocol/defs/diag_codes.yaml into
// drivelink/protocol/gen/diag_codes.hpp (tools/gen/gen.py). This header
// re-exports it as namespace `diag`, so call sites keep writing
// `diag::codes::<source>::k<Name>`. Hand-written; no code lives here.
//
// Pure C++ with no ESP-IDF dependency (the host unit tests include it).

#include "drivelink/protocol/gen/diag_codes.hpp"

namespace diag {

using drivelink::protocol::defs::diag::Code;
using drivelink::protocol::defs::diag::CodeKind;
using drivelink::protocol::defs::diag::Severity;
using drivelink::protocol::defs::diag::Source;
using drivelink::protocol::defs::diag::kCodeTable;
using drivelink::protocol::defs::diag::kSlotCount;

namespace codes = drivelink::protocol::defs::diag::codes;

} // namespace diag
