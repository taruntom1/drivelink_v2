#pragma once
#include <array>
#include <atomic>
#include <span>
#include "core/status.hpp"
#include "core_os/event.hpp"

namespace core {
inline constexpr size_t kActionBlobBytes = 512;
// Internal C++ result metadata; never serialized. Only acknowledged paused
// actions may update their own hookless live params, through ParamService.
inline constexpr size_t kActionParamUpdates = 24;
struct ActionParamUpdate { uint16_t id = 0; uint32_t bits = 0; };
struct ActionArgs {
    std::array<uint32_t,4> values{};
    std::span<const uint8_t> blob{};
};
struct ActionOut {
    std::array<uint32_t,4> values{};
    std::array<uint8_t,kActionBlobBytes> blob{};
    size_t blob_size = 0;
    std::array<ActionParamUpdate,kActionParamUpdates> params{};
    size_t param_count = 0;
};
class Instance;
enum class ActionState : uint8_t { Empty, Ready, Running, Paused, Done };
// Single producer (config task), single consumer (owning loop). All payload
// accesses are ordered by acquire/release state transitions.
struct ActionSlot {
    std::atomic<ActionState> state{ActionState::Empty};
    Instance *target = nullptr;
    uint16_t id = 0;
    bool paused = false;
    std::array<uint32_t,4> values{};
    std::array<uint8_t,kActionBlobBytes> blob{};
    size_t blob_size = 0;
    ActionOut out{};
    Status status{};
    core_os::Event completed;
};
} // namespace core
