#pragma once
#include "types.hpp"
#include <atomic>

namespace drivelink
{
// One producer per handle; write() is bounded, noexcept and allocation-free.
// The I/O thread owns wire counters and prefix-only watchdog keepalives.
class CommandGroup
{
  public:
    ~CommandGroup();
    CommandGroup(const CommandGroup &)=delete;
    CommandGroup &operator=(const CommandGroup &)=delete;
    bool write(std::span<const double> values) noexcept;
    bool writeBits(std::span<const uint32_t> values) noexcept;
    void hold(std::span<const uint32_t> values);
    void discardQueued() noexcept;
    void stop() noexcept;
    bool active() const noexcept;
    uint64_t rejected() const noexcept;
  private:
    friend class Session;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    CommandGroup(uint8_t group,uint8_t epoch,std::vector<ItemDescriptor> descriptors,
                 std::span<const uint32_t> hold,uint16_t watchdog_ms);
    bool poll(uint64_t now,Bytes &payload,uint64_t &generation);
    uint64_t queueGeneration() const noexcept;
    uint8_t id() const;
};
}
