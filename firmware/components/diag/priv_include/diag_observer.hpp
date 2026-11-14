#pragma once
#include <atomic>
#include "diag_codes.hpp"

namespace diag::detail {
using DisarmFn = void (*)(void *, Code);
// ESP32-S3 with PSRAM builds atomics as IDF's IRAM helper calls, lock-free on
// internal RAM (see core_os/atomics.hpp; diag cannot depend on core_os).
#if defined(__XTENSA__) && !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
static_assert(ATOMIC_POINTER_LOCK_FREE >= 1 && sizeof(DisarmFn) <= 4);
#else
static_assert(std::atomic<DisarmFn>::is_always_lock_free);
#endif
// Registration is setup-only. Unregistration disables new callbacks before
// waiting on the config caller; notifying never waits or takes an OS lock.
class DisarmObserver {
public:
    void install(void *ctx, DisarmFn fn) { m_ctx = ctx; m_fn.store(fn, std::memory_order_seq_cst); }
    void disable() { m_fn.store(nullptr, std::memory_order_seq_cst); }
    bool idle() const { return m_readers.load(std::memory_order_seq_cst) == 0; }
    void notify(Code code) {
        if (!code.disarms) { return; }
        m_readers.fetch_add(1, std::memory_order_seq_cst);
        const auto fn = m_fn.load(std::memory_order_seq_cst);
        if (fn) { fn(m_ctx, code); }
        m_readers.fetch_sub(1, std::memory_order_seq_cst);
    }
private:
    std::atomic<DisarmFn> m_fn{nullptr};
    std::atomic<uint32_t> m_readers{0};
    void *m_ctx = nullptr;
};
}
