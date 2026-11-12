#pragma once

// Native backend only: the fakes behind core_os/boot_info.hpp, for the tests.
// The crash counter is a process-wide variable, so it survives a `Core` being
// destroyed and built again (a simulated reboot), like RTC memory does.

#include <cstdint>

namespace core_os::native {

/// What `resetWasCrash()` answers from now on.
void setResetWasCrash(bool crash);

/// How many times `reboot()` was called.
uint32_t rebootCount();

/// Back to a power-on: no crash reset, crash counter 0, no reboots.
void resetBootInfo();

} // namespace core_os::native
