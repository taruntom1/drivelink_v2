#pragma once
#include "error.hpp"
#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>

namespace drivelink
{
using Task = std::move_only_function<void()>;
// Must enqueue FIFO, remain alive until all submitted tasks run, and not throw.
using Dispatcher = std::function<void(Task)>;
template<class T> using Completion = std::move_only_function<void(std::expected<T, Error>)>;

namespace detail
{
struct Cancellation
{
    std::mutex mutex;
    bool done = false;
    std::atomic<bool> cancelled{false}, mutation{false};
    std::function<void()> interrupt;
    std::function<void(Error)> terminate;
    std::shared_ptr<void> reservation;
    uint8_t family=0,op=0;
    bool has_context=false;
};
}
class RequestHandle
{
  public:
    RequestHandle() = default;
    explicit RequestHandle(std::shared_ptr<detail::Cancellation> state) : m_state(std::move(state)) {}
    // Stops waiting, never undoes a mutation. Destruction does not cancel.
    void cancel() const;
    explicit operator bool() const { return bool(m_state); }
  private:
    std::shared_ptr<detail::Cancellation> m_state;
};
}
