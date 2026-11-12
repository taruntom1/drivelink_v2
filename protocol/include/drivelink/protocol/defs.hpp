#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace drivelink::protocol::defs {

/// The descriptor format: what a module type exposes. `tools/gen/gen.py`
/// writes constexpr tables of these from `protocol/defs/<type>.yaml` (see
/// docs/protocol/definitions.md). The numeric values of the enums are not yet
/// a wire encoding; the describe protocol (F3) fixes that.

enum class ItemKind : uint8_t {
    Param = 0,  ///< configuration, read and write
    Signal = 1, ///< a value read in a loop tick
    Input = 2,  ///< a value the host commands through a command group
    Action = 3, ///< an operation with arguments and a result
};

/// The type of a 4-byte value slot.
enum class ValueType : uint8_t {
    Bool = 0,
    U8 = 1,
    U16 = 2,
    U32 = 3,
    I32 = 4,
    F32 = 5,
    Enum = 6, ///< u8, with named values in the descriptor
    None = 0xFF, ///< actions carry no value of their own
};

/// Bits of `ItemDesc::flags`.
enum class ItemFlag : uint8_t {
    RebootRequired = 0x01,
    Persist = 0x02,
    ReadOnly = 0x04,
    LockedWhileArmed = 0x08,
    Testgen = 0x10, ///< an input a test signal may drive
    Gpio = 0x20,    ///< a pin: a u8 `reboot_required` param; `kNoGpio` names no pin
    RefTicksAfter = 0x40, ///< a wiring param whose target ticks after this instance (YAML `ref_order: after`)
};

/// Bits of `TypeDesc::loops`: the loop classes a type may run in.
enum class LoopClass : uint8_t {
    Control = 0x01,
    Sensor = 0x02,
};

/// Where an action runs.
enum class ActionPlacement : uint8_t {
    None = 0, ///< not an action
    Loop = 1, ///< at the start of the loop tick, through a mailbox
    Paused = 2, ///< on the config task, with the instance's loop paused
};

/// Direction of an action's blob (up to 512 bytes).
enum class BlobDir : uint8_t {
    None = 0,
    In = 1,
    Out = 2,
    Both = 3,
};

struct EnumValue {
    const char *name;
    uint8_t value;
};

/// One scalar argument or result of an action.
struct ActionArg {
    const char *name;
    ValueType type;
    const char *unit;
};

/// A param, signal, input or action.
///
/// `min_bits`, `max_bits` and `default_bits` are the raw 4-byte slot bits of
/// the value, read according to `type`: a bool is 0 or 1, an unsigned integer
/// is its value, an i32 is its two's complement, an f32 is its IEEE-754 bits
/// (see `f32Bits()`) and an enum is the value of the named entry. A limit
/// that the YAML doesn't set has `has_min` or `has_max` false and its bits 0.
/// An unset default is 0.
struct ItemDesc {
    uint16_t id;
    ItemKind kind;
    ValueType type;
    uint8_t flags; ///< `ItemFlag` bits
    const char *name;
    const char *unit;        ///< "" when none
    const char *description; ///< "" when none
    uint32_t min_bits;
    uint32_t max_bits;
    uint32_t default_bits;
    bool has_min;
    bool has_max;
    std::span<const EnumValue> enum_values;
    const char *ref; ///< category of a wiring param (u32, reboot_required), or nullptr; see `wiringRef()`
    ActionPlacement runs_in;
    std::span<const ActionArg> args;
    std::span<const ActionArg> results;
    BlobDir blob;
};

/// A module type. `type_id` 0x00 is reserved for the system items.
struct TypeDesc {
    const char *name;
    uint8_t type_id;
    uint8_t loops; ///< `LoopClass` bits
    std::span<const ItemDesc> items;
};

// -- helpers -------------------------------------------------------------------

/// OR together `ItemFlag` bits.
template <typename... F> constexpr uint8_t flagBits(F... flags)
{
    return static_cast<uint8_t>((uint8_t{0} | ... | static_cast<uint8_t>(flags)));
}

/// OR together `LoopClass` bits.
template <typename... L> constexpr uint8_t loopMask(L... loops)
{
    return static_cast<uint8_t>((uint8_t{0} | ... | static_cast<uint8_t>(loops)));
}

constexpr bool hasFlag(const ItemDesc &item, ItemFlag flag)
{
    return (item.flags & static_cast<uint8_t>(flag)) != 0U;
}

constexpr bool runsIn(const TypeDesc &type, LoopClass loop)
{
    return (type.loops & static_cast<uint8_t>(loop)) != 0U;
}

// -- pins and wiring -------------------------------------------------------------

/// The value of a `gpio` param that names no pin. It is the default and
/// claims nothing.
inline constexpr uint8_t kNoGpio = 0xFF;

/// The value of an unwired `ref` param.
inline constexpr uint32_t kUnwired = 0;

/// The value of a wiring (`ref`) param that points at (type, instance):
/// `type << 8 | instance` in the low 16 bits, the upper 16 bits zero. No
/// module type has id 0x00, so 0 is free to mean unwired.
constexpr uint32_t wiringRef(uint8_t type_id, uint8_t instance)
{
    return (static_cast<uint32_t>(type_id) << 8U) | instance;
}

/// True if `value` has the wiring encoding (the upper 16 bits zero).
constexpr bool isWiringRef(uint32_t value)
{
    return (value >> 16U) == 0U;
}

constexpr uint8_t wiringRefType(uint32_t value)
{
    return static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

constexpr uint8_t wiringRefInstance(uint32_t value)
{
    return static_cast<uint8_t>(value & 0xFFU);
}

/// True if `type_name` belongs to `category`: the category is the part of the
/// type name before the first dot (`actuator` holds `actuator.pwm_dir`). A
/// name without a dot (`system`) is in no category.
constexpr bool inCategory(const char *type_name, const char *category)
{
    if (type_name == nullptr || category == nullptr || *category == '\0') {
        return false;
    }
    size_t i = 0;
    for (; category[i] != '\0'; ++i) {
        if (type_name[i] != category[i]) {
            return false;
        }
    }
    return type_name[i] == '.';
}

/// The raw IEEE-754 bits of a float, as stored in a value slot.
constexpr uint32_t f32Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

constexpr float f32FromBits(uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

/// The item of `type` with this id, or nullptr.
constexpr const ItemDesc *findItem(const TypeDesc &type, uint16_t id)
{
    for (const ItemDesc &item : type.items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

/// The type with this id in `table`, or nullptr.
constexpr const TypeDesc *findType(std::span<const TypeDesc *const> table, uint8_t type_id)
{
    for (const TypeDesc *type : table) {
        if (type->type_id == type_id) {
            return type;
        }
    }
    return nullptr;
}

/// True if no two items share an id (the generated tables `static_assert` it).
constexpr bool itemIdsUnique(std::span<const ItemDesc> items)
{
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) {
            if (items[i].id == items[j].id) {
                return false;
            }
        }
    }
    return true;
}

/// True if no two types share a type id.
constexpr bool typeIdsUnique(std::span<const TypeDesc *const> table)
{
    for (size_t i = 0; i < table.size(); ++i) {
        for (size_t j = i + 1; j < table.size(); ++j) {
            if (table[i]->type_id == table[j]->type_id) {
                return false;
            }
        }
    }
    return true;
}

} // namespace drivelink::protocol::defs
