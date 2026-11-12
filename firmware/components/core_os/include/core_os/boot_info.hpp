#pragma once

#include <cstdint>

namespace core_os {

/// True if the last reset was a crash: a panic, an interrupt watchdog or a
/// task watchdog (`esp_reset_reason()` on the device; a settable fake
/// natively, `core_os/native_boot.hpp`).
bool resetWasCrash();

/// The crash counter kept in RTC no-init memory, which survives every reset
/// but a power-on. Guarded by a magic and a complement: after a power-on (or
/// any garbage) it reads 0.
uint32_t loadCrashCount();

/// Store the crash counter (with its magic and complement). Any context; it
/// writes three words of RAM.
void storeCrashCount(uint32_t count);

/// Restart the device (`esp_restart()`; natively it only counts the calls).
void reboot();

} // namespace core_os
