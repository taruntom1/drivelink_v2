#pragma once
#include "core/core.hpp"
#include "drivelink/protocol/core_codec.hpp"
namespace core {
struct DeviceInfo {
    const char *version = "native";
    const char *build_id = "native";
    uint16_t max_payload = 1024;
    bool bench_crash = false;
};
enum class AfterReply : uint8_t { None, Reboot, Crash };
inline constexpr size_t kCaptureDoneEventBytes = 11;
// Config task: the CaptureDone event payload once per completed capture, else 0.
size_t captureDoneEvent(Core &core, std::span<uint8_t> payload);
size_t testgenDoneEvent(Core &core, std::span<uint8_t> payload);
inline uint8_t testgenStatusBits(Core &core) { return core.testgen().running() ? 16 : 0; }
// The capture state for status bits 6..7.
inline uint8_t captureStatusBits(Core &core)
{ return static_cast<uint8_t>(static_cast<uint8_t>(core.capture().snapshot().state) << 6); }
// No service scratch, queue or config-task state. Safe on the RX callback.
size_t fastDisarm(Core &core, std::span<const uint8_t> request, std::span<uint8_t> response);
// Config-task owned. Buffers belong to the caller, and are never retained.
class CommandService {
public:
    CommandService(Core &core, DeviceInfo info) : m_core(core),m_info(info) {}
    size_t handle(std::span<const uint8_t> request, std::span<uint8_t> response);
    AfterReply takeAfterReply() { auto a=m_after; m_after=AfterReply::None; return a; }
private:
    Status dispatch(drivelink::protocol::core_wire::Header h, drivelink::protocol::wire::Reader &r,
                    drivelink::protocol::wire::Writer &w);
    Status capture(drivelink::protocol::core_wire::Header h, drivelink::protocol::wire::Reader &r,
                   drivelink::protocol::wire::Writer &w);
    Status testgen(drivelink::protocol::core_wire::Header h, drivelink::protocol::wire::Reader &r,
                   drivelink::protocol::wire::Writer &w);
    Core &m_core;
    DeviceInfo m_info;
    AfterReply m_after = AfterReply::None;
    std::array<SetEntry,kMaxBatchEntries> m_sets{};
    std::array<Ref,kMaxBatchEntries> m_refs{};
    std::array<uint32_t,kMaxBatchEntries> m_values{};
    ActionOut m_action_out{};
    std::array<uint32_t,kCaptureReadBytes/4> m_capture_values{};
};
}
