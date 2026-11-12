#include "core_os/realtime.hpp"
#include "core_os/native_realtime.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
thread_local bool s_realtime = false;
thread_local uint64_t s_thread_violations = 0, s_tick_start = 0;
std::atomic<uint64_t> s_violations{0};
std::atomic<core_os::native::RealtimeViolationSink> s_sink{nullptr};
void *allocate(size_t size)
{
    // malloc is linked through __wrap_malloc in both native executables.
    if (void *p = std::malloc(size ? size : 1)) { return p; }
    throw std::bad_alloc();
}
void *alignedAllocate(size_t size, size_t alignment)
{
    core_os::recordRealtimeViolation();
#ifdef _WIN32
    void *p = _aligned_malloc(size ? size : 1, alignment);
#else
    void *p = nullptr;
    if (posix_memalign(&p, alignment, size ? size : 1) != 0) { p = nullptr; }
#endif
    if (!p) { throw std::bad_alloc(); }
    return p;
}
void alignedFree(void *p) noexcept
{
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
namespace core_os {
bool enterRealtime(bool enabled) noexcept {
    const bool old=s_realtime;
    if (!old && enabled) { s_tick_start=s_thread_violations; }
    s_realtime=old || enabled; return old;
}
void leaveRealtime(bool previous) noexcept {
    const bool completed=s_realtime && !previous;
    s_realtime=previous;
    if (completed && s_thread_violations!=s_tick_start) {
        if (const auto sink=s_sink.load()) { sink(s_thread_violations-s_tick_start); }
    }
}
bool realtimeActive() noexcept { return s_realtime; }
void recordRealtimeViolation() noexcept {
    if (s_realtime) { ++s_thread_violations; s_violations.fetch_add(1, std::memory_order_relaxed); }
}
uint64_t realtimeViolations() noexcept { return s_violations.load(std::memory_order_relaxed); }
namespace native {
RealtimeViolationSink setRealtimeViolationSink(RealtimeViolationSink sink) { return s_sink.exchange(sink); }
}
}

// Process-wide C++ allocation entry points. Record without allocation, locks,
// callbacks or exceptions on success, including aligned and nothrow forms.
void *operator new(size_t size) { return allocate(size); }
void *operator new[](size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
void *operator new(size_t size, const std::nothrow_t &) noexcept { try { return allocate(size); } catch (...) { return nullptr; } }
void *operator new[](size_t size, const std::nothrow_t &n) noexcept { return ::operator new(size, n); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void *operator new(size_t size, std::align_val_t alignment) { return alignedAllocate(size, static_cast<size_t>(alignment)); }
void *operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void *p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void *p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { alignedFree(p); }
void *operator new(size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{ try { return alignedAllocate(size, static_cast<size_t>(alignment)); } catch (...) { return nullptr; } }
void *operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t &n) noexcept
{ return ::operator new(size, alignment, n); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { alignedFree(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { alignedFree(p); }

// GNU linker wrappers also cover direct C allocator calls from project code.
extern "C" void *__real_malloc(size_t);
extern "C" void *__real_calloc(size_t, size_t);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void *__wrap_malloc(size_t size) { core_os::recordRealtimeViolation(); return __real_malloc(size); }
extern "C" void *__wrap_calloc(size_t count, size_t size) { core_os::recordRealtimeViolation(); return __real_calloc(count, size); }
extern "C" void *__wrap_realloc(void *p, size_t size) { core_os::recordRealtimeViolation(); return __real_realloc(p, size); }
