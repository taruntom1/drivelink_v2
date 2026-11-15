#pragma once

#include "time.hpp"
#include "error.hpp"
#include "async.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace drivelink
{
class GroupBuffer;
class CommandGroup;
struct ItemDescriptor;
using Bytes = std::vector<uint8_t>;
struct Frame
{
    uint8_t type;
    Bytes payload;
    ClockModel clock{};
    uint64_t generation = 0;
};
struct Reply
{
    uint8_t result, detail;
    Bytes body;
    ClockModel clock{};
    uint64_t generation=0;
};
struct SessionOptions
{
    unsigned baud = 921600;
    std::chrono::milliseconds timeout{1500};
    unsigned read_retries = 2;
    size_t event_capacity = 128;
    size_t request_capacity = 64;
    Dispatcher dispatcher{};
    bool periodic_time = true;
};
// One Asio I/O thread; sync waiters serialize except emergency DISARM.
// Timed-out IDs retire until reconnect. Callbacks only use the dispatcher.
// Endpoint: tcp://numeric-ip:port or serial://device.
class Session
{
  public:
    explicit Session(const std::string &endpoint, SessionOptions options = {});
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Reply request(uint8_t family, uint8_t op, std::span<const uint8_t> body = {});
    RequestHandle asyncRequest(uint8_t family, uint8_t op, Bytes body, Completion<Reply> completion);
    RequestHandle asyncDiagRequest(uint8_t op, Bytes body, Completion<Bytes> completion);
    RequestHandle asyncTimePing(Completion<TimeSample> completion);
    // Bounded serial service executor. Own arguments in the callable; do not
    // retain spans/references to caller storage. User code never runs on I/O.
    template<class T> RequestHandle async(std::move_only_function<T()> operation, Completion<T> completion)
    {
        auto state = std::make_shared<detail::Cancellation>();
        auto callback = std::make_shared<Completion<T>>(std::move(completion));
        auto finish = [this, state, callback](std::expected<T, Error> result) mutable {
            std::shared_ptr<void> reservation;
            { std::lock_guard lock(state->mutex); if (state->done) { return; } state->done = true; state->terminate={};state->interrupt={};reservation=std::move(state->reservation); }
            dispatchCompletion(state,[callback, reservation=std::move(reservation), result = std::move(result)]() mutable { (*callback)(std::move(result)); });
        };
        state->terminate = [finish](Error error) mutable { finish(std::unexpected(std::move(error))); };
        try {enqueueService(state, [state, operation = std::move(operation), finish = std::move(finish)]() mutable {
            if (state->cancelled) { return; }
            try {
                if constexpr (std::is_void_v<T>) { operation(); finish({}); }
                else { finish(operation()); }
            } catch (const Error &error) { finish(std::unexpected(error)); }
              catch (const std::exception &error) { finish(std::unexpected(Error(ErrorKind::Protocol, error.what()))); }
              catch (...) { finish(std::unexpected(Error(ErrorKind::Protocol,"unknown service exception"))); }
        });} catch(...) {std::lock_guard lock(state->mutex);state->terminate={};state->interrupt={};state->reservation.reset();throw;}
        return RequestHandle(state);
    }
    ClockModel clockModel() const;
    uint64_t generation() const;
    uint16_t maxPayload() const;
    using EventHandler = std::function<void(const Frame &)>;
    // Independent bounded observer queue; does not consume nextEvent().
    uint64_t observe(EventHandler handler);
    void unobserve(uint64_t id);
    uint64_t droppedNotifications() const;
    std::shared_ptr<GroupBuffer> subscribeBuffer(uint8_t group,uint8_t epoch,size_t width,size_t capacity=2048,uint64_t generation=0);
    void removeBuffer(uint8_t group);
    std::shared_ptr<CommandGroup> manageCommand(uint8_t group,uint8_t epoch,std::vector<ItemDescriptor> descriptors,
                                               std::span<const uint32_t> hold,uint16_t watchdog_ms,uint64_t generation=0);
    void stopCommand(uint8_t group);
    // OTA WRITE only, 1..4 owned request bodies. Holds ordinary call ownership
    // until every reply arrives; timeouts retire missing IDs before throwing.
    std::vector<Reply> otaWriteBatch(std::span<const Bytes> bodies);
    // Same-family/op read chunks, or OTA WRITE; 1..4 pages, replies in input order.
    std::vector<Reply> requestBatch(uint8_t family,uint8_t op,std::span<const Bytes> bodies);
    TimeSample timePing(); // no automatic retries; timestamps taken by the I/O thread
    Bytes diagRequest(uint8_t op, std::span<const uint8_t> body = {});
    // Queue a command-group payload without waiting for a peer. False means
    // the bounded stream queue is full. Request traffic retains its own limit.
    bool sendCommandGroup(uint8_t group, std::span<const uint8_t> payload);
    bool nextEvent(Frame &frame, std::chrono::milliseconds timeout);
    uint64_t droppedEvents() const;
    void setMaxPayload(uint16_t cap);
    void close(); // cancels pending requests; idempotent; never waits on a peer
  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    void enqueueService(const std::shared_ptr<detail::Cancellation> &state, Task task);
    std::shared_ptr<void> callbackSlot(bool disarm=false);
    void dispatchCompletion(const std::shared_ptr<detail::Cancellation> &state,Task task);
};
} // namespace drivelink
