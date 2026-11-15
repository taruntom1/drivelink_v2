#pragma once
#include "drivelink/protocol/defs.hpp"
#include "drivelink/protocol/sha256.hpp"
#include "drivelink/session.hpp"
#include <array>
#include <optional>
#include <variant>

namespace drivelink
{
using ValueType = protocol::defs::ValueType;
using ItemKind = protocol::defs::ItemKind;
using ActionPlacement = protocol::defs::ActionPlacement;
using BlobDir = protocol::defs::BlobDir;
// Enum values stay distinct from u8 scalars, even when their slot bits match.
struct EnumValue
{
    uint8_t value = 0;
    bool operator==(const EnumValue &) const = default;
};
using Value = std::variant<bool, uint8_t, uint16_t, uint32_t, int32_t, float, EnumValue>;
enum class GroupKind : uint8_t
{
    Subscription,
    Command
};
enum class CaptureState : uint8_t
{
    Idle,
    Armed,
    Triggered,
    Done
};
enum class TestgenState : uint8_t
{
    Idle,
    Running,
    Done
};
enum class TestgenReason : uint8_t
{
    None,
    Completed,
    Stopped,
    Disarmed
};
struct Ref
{
    uint8_t type = 0;
    uint8_t instance = 0;
    uint16_t item = 0;
    bool operator==(const Ref &) const = default;
};
struct EnumEntry
{
    uint8_t value = 0;
    std::string name;
};
struct ScalarDescriptor
{
    ValueType value_type = ValueType::None;
    std::string name;
    std::string unit;
};
struct TypeDescriptor
{
    uint8_t type = 0;
    uint8_t loop_mask = 0;
    std::string name;
};
struct ItemDescriptor
{
    uint16_t id = 0;
    ItemKind kind = ItemKind::Param;
    ValueType value_type = ValueType::None;
    uint8_t flags = 0;
    uint32_t min_bits = 0;
    uint32_t max_bits = 0;
    uint32_t default_bits = 0;
    bool has_min = false;
    bool has_max = false;
    std::string name;
    std::string unit;
    std::string description;
    std::string ref;
    ActionPlacement runs_in = ActionPlacement::None;
    BlobDir blob_direction = BlobDir::None;
    std::vector<ScalarDescriptor> args;
    std::vector<ScalarDescriptor> results;
    bool isSource() const
    {
        return (flags & 0x80) != 0;
    }
};
struct CodeDescriptor
{
    uint8_t source = 0;
    uint16_t code = 0;
    uint8_t severity = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
    std::string name;
};
struct Instance
{
    uint8_t type = 0;
    uint8_t instance = 0;
    uint8_t loop = 0;
    uint8_t state = 0;
    std::string label;
};
struct LoopInfo
{
    uint8_t id = 0;
    std::string name;
    uint32_t rate_hz = 0;
    uint8_t flags = 0;
    bool realtime() const
    {
        return (flags & 1) != 0;
    }
};
struct DeviceInfo
{
    uint8_t protocol_version = 0;
    std::string firmware_version;
    std::string build_id;
    uint32_t descriptor_hash = 0;
    uint32_t capture_capacity = 0;
    uint16_t max_payload = 0;
    uint8_t running_instances = 0;
    uint8_t flags = 0;
    std::vector<LoopInfo> loops;
};
struct OperationResult
{
    bool ok = true;
};
struct SystemResult
{
    bool ok = true;
    uint8_t result = 0;
};
struct LayoutResult
{
    bool pending_reboot = true;
};
struct AddedInstance
{
    uint8_t instance = 0;
    bool pending_reboot = true;
};
struct ParamResult
{
    uint8_t result = 0;
    bool pending_reboot = false;
    std::vector<uint32_t> values;
};
struct ActionResult
{
    std::array<uint32_t, 4> values{};
    Bytes blob;
};
struct DiagInfo
{
    uint8_t flags = 0;
    uint64_t uptime_us = 0;
    uint8_t reset_reason = 0;
    uint8_t instances_ok = 0;
    uint8_t instances_failed = 0;
    uint8_t safe_mode = 0;
    uint32_t records_dropped = 0;
    uint32_t events_tx_dropped = 0;
    std::string firmware_version;
};
struct DiagEntry
{
    uint8_t source = 0;
    uint8_t instance = 0;
    uint16_t code = 0;
    uint8_t state = 0;
    uint32_t count = 0;
};
struct Group
{
    GroupKind kind = GroupKind::Subscription;
    uint8_t id = 0;
    uint8_t channel = 0;
    bool configured = false;
    bool enabled = false;
    uint8_t loop = 255;
    uint8_t epoch = 0;
    uint16_t divider = 1;
    std::vector<Ref> refs;
};
struct GroupList
{
    std::vector<Group> groups;
    uint32_t subscription_capacity = 0;
    uint32_t command_capacity = 0;
    uint32_t entry_capacity = 0;
};
struct GroupMutation
{
    uint8_t epoch = 0;
    uint8_t channel = 0;
};
struct CaptureTrigger
{
    uint8_t kind = 0;
    Ref ref{};
    uint8_t edge = 0;
    uint32_t value = 0;
};
struct CaptureStatus
{
    CaptureState state = CaptureState::Idle;
    bool configured = false;
    bool irregular = false;
    uint8_t loop = 0;
    uint16_t divider = 0;
    uint32_t samples = 0;
    uint32_t pre_trigger = 0;
    CaptureTrigger trigger;
    uint32_t recorded = 0;
    std::optional<uint32_t> trigger_index;
    uint32_t period_us = 0;
    uint64_t sample_period_us = 0;
    uint64_t trigger_timestamp_us = 0;
    std::vector<Ref> refs;
    MappedTime time{};
};
struct CapturePage
{
    uint32_t first = 0;
    uint16_t count = 0;
    std::vector<std::vector<uint32_t>> samples;
};
struct CaptureData
{
    CaptureStatus status;
    std::vector<std::vector<uint32_t>> data;
    std::vector<MappedTime> times{};
};
struct TestgenConfig
{
    uint8_t loop = 0;
    Ref target{};
    uint8_t waveform = 0;
    float offset = 0;
    float amplitude = 0;
    float frequency_start = 0;
    float frequency_end = 0;
    uint32_t duration_ms = 1000;
    uint32_t repeat = 1;
};
struct TestgenStarted
{
    uint8_t loop = 0;
    uint32_t run = 0;
};
struct TestgenStatus
{
    uint8_t loop = 0;
    TestgenState state = TestgenState::Idle;
    TestgenReason reason = TestgenReason::None;
    uint32_t run = 0;
    uint32_t elapsed_ms = 0;
    uint32_t repetitions = 0;
    float value = 0;
    Ref target{};
    uint8_t waveform = 0;
    float offset = 0;
    float amplitude = 0;
    float frequency_start = 0;
    float frequency_end = 0;
    uint32_t duration_ms = 0;
    uint32_t repeat = 0;
};
struct OtaStatus
{
    uint8_t state = 0;
    uint8_t running_slot = 0;
    uint8_t boot_slot = 0;
    bool pending_confirm = false;
    uint32_t size = 0;
    uint32_t offset = 0;
    uint32_t capacity = 0;
    uint16_t chunk_bytes = 0;
    uint8_t window = 0;
    int32_t backend_error = 0;
    uint8_t stage = 0;
    protocol::Sha256Digest sha256{};
    protocol::Sha256Digest elf_sha256{};
};
struct LoopOverruns
{
    uint8_t loop_id = 0;
    uint32_t overruns = 0;
};
struct StatusFrame
{
    uint8_t epoch = 0;
    uint16_t counter = 0;
    uint64_t timestamp_us = 0;
    uint8_t arm_state = 0;
    uint8_t flags = 0;
    uint8_t active_errors = 0;
    std::array<uint8_t, 2> cpu_load{};
    std::vector<LoopOverruns> overruns;
    uint8_t last_disarm_reason = 0;
    MappedTime time{};
};
struct CaptureDone
{
    uint32_t recorded = 0;
    uint32_t trigger_index = 0;
    bool irregular = false;
};
struct TestgenDone
{
    uint8_t loop = 0;
    uint32_t run = 0;
    TestgenReason reason = TestgenReason::None;
};
struct DiagRecord
{
    uint16_t sequence = 0;
    uint8_t flags = 0;
    uint64_t timestamp_us = 0;
    uint8_t severity = 0;
    uint8_t source = 0;
    uint8_t instance = 0;
    uint16_t code = 0;
    int32_t error = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    MappedTime time{};
};
struct CounterDelta
{
    uint8_t source = 0;
    uint8_t instance = 0;
    uint16_t code = 0;
    uint32_t delta = 0;
    uint32_t total = 0;
};
struct DiagCounters
{
    uint16_t sequence = 0;
    uint8_t flags = 0;
    uint64_t timestamp_us = 0;
    std::vector<CounterDelta> entries;
    MappedTime time{};
};
struct DiagCleared
{
    uint16_t sequence = 0;
    uint8_t flags = 0;
    uint64_t timestamp_us = 0;
    uint8_t source = 0;
    uint8_t instance = 0;
    uint16_t code = 0;
    MappedTime time{};
};
struct BootEvent
{
    uint16_t sequence = 0;
    uint8_t flags = 0;
    uint64_t timestamp_us = 0;
    uint8_t reset_reason = 0;
    uint8_t instances_ok = 0;
    uint8_t instances_failed = 0;
    uint8_t safe_mode = 0;
    MappedTime time{};
};
struct SubscriptionSample
{
    uint8_t group = 0;
    uint8_t epoch = 0;
    uint16_t counter = 0;
    uint64_t timestamp_us = 0;
    std::vector<uint32_t> values;
    std::vector<Value> typed_values;
    MappedTime time{};
};
using Event =
    std::variant<StatusFrame, CaptureDone, TestgenDone, DiagRecord, DiagCounters, DiagCleared, BootEvent, Frame>;
// Slot conversion is explicit: param/action/capture wire results retain bits.
// Descriptors are discovered from the device; there is no host module table.
Value decodeValue(ValueType type, uint32_t bits);
uint32_t valueBits(const Value &value);
uint32_t parseValue(ValueType type, const std::string &text, std::span<const EnumEntry> enums = {});
inline uint32_t parseValue(const ItemDescriptor &d, const std::string &text, std::span<const EnumEntry> enums = {})
{
    return parseValue(d.value_type, text, enums);
}
inline uint32_t parseValue(const ScalarDescriptor &d, const std::string &text, std::span<const EnumEntry> enums = {})
{
    return parseValue(d.value_type, text, enums);
}
} // namespace drivelink
