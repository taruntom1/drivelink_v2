#pragma once

#include <memory>
#include <span>
#include "core/param_service.hpp"

namespace core {

enum class GroupKind : uint8_t { Subscription, Command };
inline constexpr size_t kMaxGroupEntries = 64;
inline constexpr size_t kGroupStreamPrefix = 12;
inline constexpr size_t kGroupStreamBytes = kGroupStreamPrefix + 4 * kMaxGroupEntries;

// A resolved scalar. The storage is read only on its owning loop. F4c can
// reuse latchSignals for a coherent list of values at the end of a tick.
struct BoundSignal {
    Ref ref{};
    const defs::ItemDesc *item = nullptr;
    void *storage = nullptr;
    int64_t *receipt_us = nullptr;
    detail::Binding *gate = nullptr;
};
void latchSignals(std::span<const BoundSignal> signals, std::span<uint32_t> values);
// Resolve a bound signal or input (inputs only when `inputs_only`) of a
// running instance owned by `loop`. A loop instance owns its own statistics.
// Shared by groups and capture; config callers only.
Status bindSignal(const detail::InstanceTable &table, const Ref &ref, uint8_t loop, bool inputs_only,
                  BoundSignal &out);

struct GroupInfo {
    bool configured = false;
    bool enabled = false;
    uint8_t loop = kNoLoop;
    uint8_t epoch = 0;
    uint16_t divider = 1;
    uint8_t count = 0;
    std::array<Ref, kMaxGroupEntries> refs{};
};

// Config calls have one caller. receiveCommand is RX-owned; beginTick/endTick
// run on each loop; takeStream has one transport caller. All shared slots use
// bounded, single-attempt atomic ownership transfers, never an OS lock or a
// spin wait. A busy RX/publication slot drops that frame/sample.
class GroupService {
public:
    GroupService(const detail::InstanceTable &table, std::span<Loop *> loops,
                 uint8_t subscriptions, uint8_t commands, uint8_t entries);
    ~GroupService();
    GroupService(const GroupService &) = delete;
    GroupService &operator=(const GroupService &) = delete;

    // For the RAM estimate: one group of either kind, and what is there anyway.
    static size_t slotBytes();
    static size_t baseBytes();

    size_t capacity(GroupKind kind) const;
    uint8_t entryCapacity() const;
    const GroupInfo *info(GroupKind kind, uint8_t id) const;
    Status configure(GroupKind kind, uint8_t id, uint8_t loop, uint16_t divider,
                     std::span<const Ref> refs, bool armed = false);
    Status enable(uint8_t id, bool enabled);
    Status clear(GroupKind kind, uint8_t id, bool armed = false);

    // Split staging API for deterministic tests; complete must follow success.
    Status stageConfig(GroupKind kind, uint8_t id, uint8_t loop, uint16_t divider,
                       std::span<const Ref> refs, bool armed = false);
    Status complete(GroupKind kind, uint8_t id, uint32_t timeout_us);

    bool receiveCommand(uint8_t id, std::span<const uint8_t> payload, int64_t received_us);
    // Device loops supply their coherent arm state and arm instant; pre-arm
    // mailboxes are never applied.
    void beginTick(uint8_t loop, bool armed = false, int64_t armed_at_us = 0);
    void endTick(uint8_t loop, int64_t timestamp_us);
    size_t takeStream(uint8_t id, std::span<uint8_t> payload);
    // False means the snapshot was unavailable; the caller must not treat
    // contention as a missing command. ESP32 needs no 64-bit atomic here.
    bool lastCommandUs(uint8_t id, int64_t &received_us) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace core
