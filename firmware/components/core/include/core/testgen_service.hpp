#pragma once

#include <memory>
#include "core/arm_state.hpp"
#include "core/group_service.hpp"

namespace core {
class CaptureService;
using TestgenWaveform = defs::TestgenWaveform;
using TestgenState = defs::TestgenState;
using TestgenReason = defs::TestgenReason;

struct TestgenConfig {
    uint8_t loop = kNoLoop;
    Ref target{};
    TestgenWaveform waveform = TestgenWaveform::Step;
    float offset = 0, amplitude = 0, frequency_start = 0, frequency_end = 0;
    uint32_t duration_ms = 1000, repeat = 1;
};
struct TestgenSnapshot {
    TestgenState state = TestgenState::Idle;
    TestgenReason reason = TestgenReason::None;
    uint32_t run = 0, elapsed_ms = 0, repetitions = 0, value_bits = 0;
};

// Pure waveform evaluation; elapsed is within one repetition, in seconds.
// Caller validates config. Double intermediates keep every finite f32 input
// representable until final descriptor/f32 clamping.
double testgenValue(const TestgenConfig &config, double elapsed_s);

// One fixed slot per loop. Config calls require one caller. Only owning-loop
// beginTick/sample touch bindings, gates and active state. Snapshots use
// lock-free 32-bit atomics; terminal events cannot be overwritten by START.
class TestgenService {
public:
    TestgenService(const detail::InstanceTable &table, std::span<Loop *> loops,
                   ArmState &arm, CaptureService &capture);
    ~TestgenService();
    static size_t slotBytes(); // per loop of the capacity, for the RAM estimate
    Status start(const TestgenConfig &config, uint32_t &run);
    Status stop(uint8_t loop);
    Status snapshot(uint8_t loop, TestgenSnapshot &out) const;
    const TestgenConfig *config(uint8_t loop) const;
    bool running() const;
    bool takeDone(uint8_t &loop, TestgenSnapshot &out);
    // Split staging for deterministic tests; complete follows successful stage.
    Status stageStart(const TestgenConfig &config, uint32_t &run);
    Status stageStop(uint8_t loop);
    Status complete(uint8_t loop, uint32_t timeout_us);
    void beginTick(uint8_t loop, int64_t now_us, uint32_t arm_word);
    void sample(uint8_t loop, int64_t now_us);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
