#pragma once

#include <cstdint>
#include <type_traits>

#include "core/status.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core {

namespace defs = drivelink::protocol::defs;

/// The result of checking a value against its item.
struct ValueCheck {
    bool ok;
    uint32_t applied_bits; ///< the value to apply (clamped to min/max); 0 when rejected
    Detail detail;         ///< why it was rejected; None when ok
};

/// Check the 4-byte slot `bits` against `item` (a param, signal or input).
///
/// Rejected (nothing to clamp): a bool other than 0 or 1, a u8 or u16 that
/// does not fit its type, an enum value that is not listed, a NaN or infinite
/// f32. A value within its type but outside min/max is clamped, for u8, u16,
/// u32, i32 and f32. Real-time safe: no allocation, bounded by the enum size.
ValueCheck checkValue(const defs::ItemDesc &item, uint32_t bits);

/// Write the slot `bits` into storage of `type` (bool, u8, u16, ...). Real-time
/// safe. `bits` must already have passed `checkValue`.
void storeValue(defs::ValueType type, void *dst, uint32_t bits);

/// Read storage of `type` into slot bits. Real-time safe.
uint32_t loadValue(defs::ValueType type, const void *src);

/// The value type a C++ storage type binds to: bool, uint8_t, uint16_t,
/// uint32_t, int32_t, float, or an enum class with a uint8_t base (Enum).
template <typename T> constexpr defs::ValueType valueTypeOf()
{
    if constexpr (std::is_same_v<T, bool>) {
        return defs::ValueType::Bool;
    } else if constexpr (std::is_same_v<T, uint8_t>) {
        return defs::ValueType::U8;
    } else if constexpr (std::is_same_v<T, uint16_t>) {
        return defs::ValueType::U16;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return defs::ValueType::U32;
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return defs::ValueType::I32;
    } else if constexpr (std::is_same_v<T, float>) {
        return defs::ValueType::F32;
    } else if constexpr (std::is_enum_v<T>) {
        static_assert(std::is_same_v<std::underlying_type_t<T>, uint8_t>, "an enum binds only with a uint8_t base");
        return defs::ValueType::Enum;
    } else {
        static_assert(sizeof(T) == 0, "bind storage must be bool, u8, u16, u32, i32, float or a u8 enum class");
        return defs::ValueType::None;
    }
}

} // namespace core
