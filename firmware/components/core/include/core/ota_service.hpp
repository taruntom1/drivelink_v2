#pragma once
#include "core/status.hpp"
#include "core_os/ota.hpp"
#include "drivelink/protocol/ota_image.hpp"
#include <atomic>

namespace core {
class Core;
// Config-caller owned except the atomic ARM gate. No loop/RX flash access.
class OtaService {
public:
    explicit OtaService(Core &core) : m_core(core) {}
    void initialize();
    Status begin(uint32_t size,const drivelink::protocol::Sha256Digest &digest);
    Status write(uint32_t offset,std::span<const uint8_t> bytes);
    Status finish();
    Status abort();
    Status confirm();
    Status refresh();
    bool blocksArm() const { return m_gate.load(std::memory_order_acquire); }
    bool transaction() const { return m_state!=State::Idle; }
    bool pending() const { return m_boot.pending; }
    using State=drivelink::protocol::defs::OtaState;
    using Stage=drivelink::protocol::defs::OtaStage;
    State state() const { return m_state; }
    uint32_t size() const { return m_size; }
    uint32_t offset() const { return m_offset; }
    const auto &digest() const { return m_digest; }
    const auto &bootInfo() const { return m_boot; }
    core_os::Err lastError() const { return m_error; }
    Stage stage() const { return m_stage; }
    core_os::Ota &platform() { return m_ota; } // native fault/reset fixtures
private:
    Status disarmed() const;
    Status fail(core_os::Err err,Stage stage);
    void gate();
    Core &m_core;
    core_os::Ota m_ota;
    core_os::OtaBootInfo m_boot;
    State m_state=State::Idle;
    std::atomic<bool> m_gate{true};
    uint32_t m_size=0,m_offset=0;
    core_os::Err m_error=0;
    Stage m_stage=Stage::None;
    drivelink::protocol::Sha256Digest m_digest{};
    drivelink::protocol::Sha256 m_hash;
    std::array<uint8_t,drivelink::protocol::kOtaChunkBytes> m_scratch{};
};
}
