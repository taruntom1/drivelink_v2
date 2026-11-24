#pragma once

// A fake driver backend for mod_motor/hardware.hpp. The native tests link it
// in place of src/idf/motor_hw_idf.cpp. Fixed storage, atomics only: the
// modules call write/forceLow/releaseForce/read inside real-time ticks and
// from a monitor thread, so nothing here may allocate or block.

#include "core_os/types.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fake_motor_hw {

struct Output {
    uint8_t pwm_pin = 0, dir_pin = 0;
    uint32_t pwm_freq_hz = 0;
    std::atomic<bool> open{false};
    std::atomic<bool> forced{false};
    std::atomic<float> duty{0};          // last successful write
    std::atomic<uint32_t> writes{0}, forces{0}, releases{0};
    std::atomic<bool> fail_write{false}, fail_force{false}, fail_release{false};
    /// One-shot: called inside releaseForce() before the release lands, to
    /// place a concurrent monitor stop exactly in the race window.
    void (*during_release)(void *) = nullptr;
    void *during_release_ctx = nullptr;
    /// The duty the pins drive now: zero while forced.
    float effective() const { return forced.load() ? 0.F : duty.load(); }
};

struct Counter {
    uint8_t pin_a = 0, pin_b = 0;
    bool pullup = false;
    uint16_t glitch_ns = 0;
    std::atomic<bool> open{false};
    std::atomic<int32_t> count{0};
    std::atomic<bool> fail_read{false};
};

inline constexpr size_t kMax = 8;
/// In open order. A failed open takes no slot.
extern std::array<Output, kMax> outputs;
extern std::array<Counter, kMax> counters;
extern size_t output_count, counter_count;
/// The error the next openPwmDir / openQuadrature returns (0: succeed).
extern core_os::Err fail_next_pwm_open, fail_next_quadrature_open;

/// Forget every output and counter. Call with no module alive.
void reset();

} // namespace fake_motor_hw
