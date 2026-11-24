// IDF only: what the platform decides in CoreConfig: the link's frame size, and
// the heap the rest of the firmware takes (all from Kconfig). The core never sees
// sdkconfig.h or a CONFIG_ symbol. The limits and the link are system settings.

#include "core/capture_service.hpp"
#include "core/core_config.hpp"
#include "core/core_link.hpp"
#include "core_os/capture_memory.hpp"
#include "diag_reporter.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "sdkconfig.h"
#include "uart_comm.hpp"

namespace core {

namespace {
/// Task control blocks, timers and small driver state of what comes after Core.
constexpr size_t kAfterCoreExtra = 4096;
} // namespace

CoreConfig coreConfigFromKconfig()
{
    CoreConfig cfg;
    cfg.group_entries_cap = drivelink::protocol::core_wire::groupEntryCapacity(CONFIG_UART_COMM_MAX_PAYLOAD, kMaxGroupEntries);
    cfg.capture_refs_cap = drivelink::protocol::core_wire::captureRefCapacity(CONFIG_UART_COMM_MAX_PAYLOAD, kMaxCaptureRefs);
    cfg.ram.stream_slot_bytes = uart_comm::UartComm::streamSlotBytes(0);
    cfg.ram.after_core_bytes = CoreLink::ramBytes() + diag::Reporter::ramBytes() + kAfterCoreExtra +
                               (core_os::captureUsesInternalRam() ? core_os::kCaptureFallbackBytes : 0U);
    return cfg;
}

} // namespace core
