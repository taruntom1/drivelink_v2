#include "core/value.hpp"

#include <cmath>
#include <cstring>

namespace core {

namespace {

constexpr ValueCheck reject(Detail detail)
{
    return ValueCheck{false, 0U, detail};
}

constexpr ValueCheck accept(uint32_t bits)
{
    return ValueCheck{true, bits, Detail::None};
}

uint32_t clampUnsigned(const defs::ItemDesc &item, uint32_t v)
{
    if (item.has_min && v < item.min_bits) {
        return item.min_bits;
    }
    if (item.has_max && v > item.max_bits) {
        return item.max_bits;
    }
    return v;
}

uint32_t clampSigned(const defs::ItemDesc &item, uint32_t bits)
{
    const auto v = static_cast<int32_t>(bits);
    if (item.has_min && v < static_cast<int32_t>(item.min_bits)) {
        return item.min_bits;
    }
    if (item.has_max && v > static_cast<int32_t>(item.max_bits)) {
        return item.max_bits;
    }
    return bits;
}

} // namespace

ValueCheck checkValue(const defs::ItemDesc &item, uint32_t bits)
{
    switch (item.type) {
    case defs::ValueType::Bool:
        return (bits <= 1U) ? accept(bits) : reject(Detail::BadValue);
    case defs::ValueType::U8:
        return (bits <= 0xFFU) ? accept(clampUnsigned(item, bits)) : reject(Detail::BadValue);
    case defs::ValueType::U16:
        return (bits <= 0xFFFFU) ? accept(clampUnsigned(item, bits)) : reject(Detail::BadValue);
    case defs::ValueType::U32:
        return accept(clampUnsigned(item, bits));
    case defs::ValueType::I32:
        return accept(clampSigned(item, bits));
    case defs::ValueType::F32: {
        const float v = defs::f32FromBits(bits);
        if (!std::isfinite(v)) {
            return reject(Detail::NonFiniteFloat);
        }
        if (item.has_min && v < defs::f32FromBits(item.min_bits)) {
            return accept(item.min_bits);
        }
        if (item.has_max && v > defs::f32FromBits(item.max_bits)) {
            return accept(item.max_bits);
        }
        return accept(bits);
    }
    case defs::ValueType::Enum:
        if (bits <= 0xFFU) {
            for (const defs::EnumValue &e : item.enum_values) {
                if (e.value == bits) {
                    return accept(bits);
                }
            }
        }
        return reject(Detail::BadEnum);
    case defs::ValueType::None:
        break;
    }
    return reject(Detail::WrongKind); // an action has no value
}

void storeValue(defs::ValueType type, void *dst, uint32_t bits)
{
    switch (type) {
    case defs::ValueType::Bool: {
        const bool v = bits != 0U;
        std::memcpy(dst, &v, sizeof v);
        break;
    }
    case defs::ValueType::U8:
    case defs::ValueType::Enum: {
        const auto v = static_cast<uint8_t>(bits);
        std::memcpy(dst, &v, sizeof v);
        break;
    }
    case defs::ValueType::U16: {
        const auto v = static_cast<uint16_t>(bits);
        std::memcpy(dst, &v, sizeof v);
        break;
    }
    case defs::ValueType::U32:
    case defs::ValueType::I32:
    case defs::ValueType::F32:
        std::memcpy(dst, &bits, sizeof bits); // same size and representation as the slot
        break;
    case defs::ValueType::None:
        break;
    }
}

uint32_t loadValue(defs::ValueType type, const void *src)
{
    switch (type) {
    case defs::ValueType::Bool: {
        bool v = false;
        std::memcpy(&v, src, sizeof v);
        return v ? 1U : 0U;
    }
    case defs::ValueType::U8:
    case defs::ValueType::Enum: {
        uint8_t v = 0;
        std::memcpy(&v, src, sizeof v);
        return v;
    }
    case defs::ValueType::U16: {
        uint16_t v = 0;
        std::memcpy(&v, src, sizeof v);
        return v;
    }
    case defs::ValueType::U32:
    case defs::ValueType::I32:
    case defs::ValueType::F32: {
        uint32_t v = 0;
        std::memcpy(&v, src, sizeof v);
        return v;
    }
    case defs::ValueType::None:
        break;
    }
    return 0U;
}

} // namespace core
