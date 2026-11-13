#pragma once

#include <atomic>
#include <cstddef>

namespace core_os {

/// True when atomics are out-of-line library calls. ESP-IDF builds the
/// ESP32-S3 with `-mdisable-hardware-atomics` whenever PSRAM is enabled (the
/// hidden `CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND`): S32C1I does not work on
/// PSRAM addresses. Every atomic then calls IDF's IRAM helpers (`noflash`, so
/// safe from IRAM ISRs and during flash writes), which still use the hardware
/// S32C1I for internal RAM and take a critical section only for a PSRAM
/// address. Rule (F7a): no atomic or shared state ever lives in PSRAM; PSRAM is
/// caps-alloc only and holds the plain capture ring.
#if defined(__XTENSA__) && !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
inline constexpr bool kAtomicsOutOfLine = true;
#else
inline constexpr bool kAtomicsOutOfLine = false;
#endif

/// The lock-free guarantee the core relies on: always lock-free, or (under the
/// workaround above) a word-sized atomic that the IDF helpers serve with
/// S32C1I on internal RAM.
template <typename T> constexpr bool lockFreeAtomic()
{
    return std::atomic<T>::is_always_lock_free ||
           (kAtomicsOutOfLine && ATOMIC_INT_LOCK_FREE >= 1 && sizeof(T) <= 4);
}

} // namespace core_os
