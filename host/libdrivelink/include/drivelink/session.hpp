#pragma once

#include "time.hpp"
#include "error.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace drivelink
{
using Bytes = std::vector<uint8_t>;
struct Frame
{
    uint8_t type;
    Bytes payload;
};
struct Reply
{
    uint8_t result, detail;
    Bytes body;
};
struct SessionOptions
{
    unsigned baud = 921600;
    std::chrono::milliseconds timeout{1500};
    unsigned read_retries = 2;
    size_t event_capacity = 128;
};
// Thread-safe synchronous facade over one Asio I/O thread. Calls serialize;
// timed-out sequence IDs are retired until reconnect to reject stale replies.
// Endpoint: tcp://numeric-ip:port or serial://device. No callbacks on callers.
class Session
{
  public:
    explicit Session(const std::string &endpoint, SessionOptions options = {});
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Reply request(uint8_t family, uint8_t op, std::span<const uint8_t> body = {});
    // OTA WRITE only, 1..4 owned request bodies. Holds ordinary call ownership
    // until every reply arrives; timeouts retire missing IDs before throwing.
    std::vector<Reply> otaWriteBatch(std::span<const Bytes> bodies);
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
};
} // namespace drivelink
