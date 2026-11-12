#pragma once

#include <cstdint>

namespace core {

/// The outcome of a core operation. F3b moves this enum into
/// `protocol/defs/registry.yaml` with wire numbers and keeps the names, so
/// the numeric values here mean nothing yet.
enum class Result : uint8_t {
    Ok = 0,
    InvalidArgument,
    NotFound,
    Exists,
    NoMemory,
    Busy,
    Armed,         ///< not allowed while armed
    NotArmed,      ///< only allowed while armed
    PendingReboot, ///< accepted into the pending layout: takes effect after SAVE and a reboot
    Fault,         ///< an internal failure (see the diag record)
};

/// Why an operation failed: which check said no. `None` on success. Like
/// `Result`, F3b gives it wire numbers in `registry.yaml` and keeps the names.
enum class Detail : uint8_t {
    None = 0,
    // References and kinds
    UnknownRef,  ///< no such (type, instance) or no such item id
    UnknownType, ///< the type is not registered
    WrongKind,   ///< the item is not of the kind the operation needs
    WrongType,   ///< a bind's C++ type does not match the item's value type
    AlreadyBound,
    NullStorage,
    // Param writes
    ReadOnly,
    LockedWhileArmed,
    Unsupported, ///< not supported (a reboot_required system param)
    BadValue,    ///< not a value of the item's type (a bool of 2, a u8 of 300)
    BadEnum,     ///< not one of the enum's listed values
    NonFiniteFloat,
    CrossLoopBatch, ///< a batch spans domains: loops, system, offline instances or the pending layout
    StagingBusy,    ///< the loop's staging slot holds another batch
    Withdrawn,      ///< the loop did not take the batch in time; nothing was applied
    BatchTooLarge,
    DuplicateRef,   ///< the same Ref twice in one batch
    BufferTooSmall, ///< the caller's output span is shorter than its input
    TimerFailed,    ///< a loop timer could not be restarted at the new rate
    // Layout
    LoopMismatch, ///< the type may not run in that loop
    LabelInUse,
    BadLabel,
    GpioConflict,   ///< a pin is used twice on the device
    GpioNotAllowed, ///< a pin the device does not allow (CoreConfig::gpio_allowed_mask)
    BadRef,         ///< a wiring value that is malformed, names no instance, or one of the wrong category
    WiringCycle,    ///< the ref graph or the tick graph has a cycle through this instance
    Referenced,     ///< the instance is wired to by another one
    DependencyMissing, ///< an instance this one is wired to is invalid
    SafeMode,
    Capacity, ///< CORE_MAX_INSTANCES reached
    NotAtBoot, ///< a boot-only operation after the loops started
    StartFailed,
    // Persistence
    NoSavedConfig, ///< NVS holds no complete configuration
    StorageFailed, ///< an NVS write or erase failed (see core.NvsWriteFailed)
    // Registry
    ReservedType,  ///< type id 0x00 belongs to the system
    DuplicateType,
    NoFactory,     ///< a module type with a null `create`
};

/// A result, the detail, and the index of the failing entry in a batch (0
/// when the operation has no entries).
struct Status {
    Result result = Result::Ok;
    Detail detail = Detail::None;
    uint16_t index = 0;

    constexpr bool ok() const { return result == Result::Ok; }
    /// Ok, or PendingReboot: the operation was accepted (a pending one takes
    /// effect after SAVE and a reboot).
    constexpr bool accepted() const { return result == Result::Ok || result == Result::PendingReboot; }

    static constexpr Status success() { return Status{}; }
    static constexpr Status fail(Result result, Detail detail, uint16_t index = 0)
    {
        return Status{result, detail, index};
    }
};

} // namespace core
