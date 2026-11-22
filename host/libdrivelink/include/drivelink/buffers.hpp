#pragma once
#include "session.hpp"
#include <array>
#include <atomic>
#include <memory>

namespace drivelink
{
// Fixed storage for realtime readers. Slot bits follow the discovered group's
// reference order. latest() and pop() neither allocate nor take a mutex.
struct GroupSample
{
    uint8_t group=0, epoch=0, count=0;
    uint16_t counter=0;
    uint64_t device_us=0, generation=0;
    double host_us=0, uncertainty_us=0;
    bool mapped=false;
    std::array<uint32_t,64> values{};
};
class GroupBuffer
{
  public:
    explicit GroupBuffer(uint8_t group,uint8_t epoch,size_t width,size_t capacity=2048);
    ~GroupBuffer();
    GroupBuffer(const GroupBuffer &)=delete;
    GroupBuffer &operator=(const GroupBuffer &)=delete;
    bool latest(GroupSample &sample) const noexcept;
    bool pop(GroupSample &sample) noexcept;
    uint64_t dropped() const noexcept;
    uint64_t malformed() const noexcept;
    void invalidate() noexcept;
  private:
    friend class Session;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    void receive(const Frame &frame) noexcept;
};
}
