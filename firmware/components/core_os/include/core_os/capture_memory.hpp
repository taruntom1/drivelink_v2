#pragma once

#include <cstddef>

namespace core_os {

/// The internal ring a board without PSRAM falls back to.
inline constexpr size_t kCaptureFallbackBytes = 32768;

/// The largest capture ring this device can allocate, at most `limit_bytes`:
/// the largest free PSRAM block when PSRAM was found, else the internal
/// fallback. Call once at setup; the core reports the result in GET_INFO.
size_t captureCapacity(size_t limit_bytes);

/// True when the ring comes from internal RAM (no PSRAM found).
bool captureUsesInternalRam();

/// Allocate a capture ring from PSRAM (`MALLOC_CAP_SPIRAM`), or from internal
/// RAM when the device has none. Configure time only (the capture's CONFIG on
/// the config task), never from a loop. @return nullptr on failure.
void *captureAlloc(size_t bytes);

/// Free a ring from `captureAlloc`. Configure time only. Null is ignored.
void captureFree(void *ring);

} // namespace core_os
