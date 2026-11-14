#pragma once

#include "core_os/detail/backend_types.hpp"

namespace core_os {

/// A plain (non-recursive) mutex with static storage: it cannot fail to
/// construct. For short sections on non-real-time paths only. A real-time
/// path may use `tryLock()` and nothing else.
class Mutex {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    /// Wait until the mutex is free. Never from a real-time path.
    void lock();
    /// Take the mutex if it is free. Never waits.
    bool tryLock();
    void unlock();

private:
    detail::MutexState m_state;
};

/// Scoped `lock()` / `unlock()`.
class LockGuard {
public:
    explicit LockGuard(Mutex &mutex) : m_mutex(mutex) { m_mutex.lock(); }
    ~LockGuard() { m_mutex.unlock(); }

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    Mutex &m_mutex;
};

} // namespace core_os
