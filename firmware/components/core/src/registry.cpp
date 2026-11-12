#include "core/registry.hpp"

#include "drivelink/protocol/gen/registry.hpp"

namespace core {

Status Registry::init(std::span<const ModuleType *const> types)
{
    m_types = {};
    for (size_t i = 0; i < types.size(); ++i) {
        const auto index = static_cast<uint16_t>(i);
        const ModuleType *type = types[i];
        if (type == nullptr) {
            return Status::fail(Result::InvalidArgument, Detail::UnknownType, index);
        }
        if (type->desc.type_id == drivelink::protocol::defs::kSystemTypeId) {
            return Status::fail(Result::InvalidArgument, Detail::ReservedType, index);
        }
        if (type->create == nullptr) {
            return Status::fail(Result::InvalidArgument, Detail::NoFactory, index);
        }
        for (size_t j = 0; j < i; ++j) {
            if (types[j]->desc.type_id == type->desc.type_id) {
                return Status::fail(Result::InvalidArgument, Detail::DuplicateType, index);
            }
        }
    }
    m_types = types;
    return Status::success();
}

const ModuleType *Registry::find(uint8_t type_id) const
{
    for (const ModuleType *type : m_types) {
        if (type->desc.type_id == type_id) {
            return type;
        }
    }
    return nullptr;
}

bool descIsInTable(const ModuleType &type, std::span<const drivelink::protocol::defs::TypeDesc *const> table)
{
    return drivelink::protocol::defs::findType(table, type.desc.type_id) == &type.desc;
}

} // namespace core
