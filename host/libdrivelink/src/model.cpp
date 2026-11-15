#include "drivelink/model.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <set>

namespace drivelink
{
namespace
{
[[noreturn]] void invalid(const char *message)
{
    throw Error(ErrorKind::Protocol, std::string("invalid device snapshot: ") + message);
}

std::map<uint16_t, Value> params(Client &client, const ModelType &type,
                               uint8_t instance, bool pending, size_t batch_size)
{
    std::vector<Ref> refs;
    for (const auto &[id, item] : type.items)
    {
        if (item.descriptor.kind == ItemKind::Param)
        {
            refs.push_back({type.descriptor.type, instance, id});
        }
    }
    std::map<uint16_t, Value> result;
    for (size_t first = 0; first < refs.size(); first += batch_size)
    {
        const auto batch = std::span<const Ref>(refs).subspan(first, std::min(batch_size, refs.size() - first));
        const auto values = client.get(batch, pending);
        for (size_t i = 0; i < batch.size(); ++i)
        {
            const auto &item = type.items.at(batch[i].item);
            auto value = decodeValue(item.descriptor.value_type, values.values.at(i));
            if (const auto *e = std::get_if<EnumValue>(&value))
            {
                if (std::none_of(item.enums.begin(), item.enums.end(),
                                 [e](const EnumEntry &entry) { return entry.value == e->value; }))
                {
                    invalid("parameter enum value is not described");
                }
            }
            result.emplace(batch[i].item, std::move(value));
        }
    }
    return result;
}

ModelLayout layout(Client &client, const DeviceSnapshot &snapshot, const std::vector<Instance> &instances,
                   bool pending, size_t batch_size)
{
    ModelLayout result;
    for (const auto &instance : instances)
    {
        if (instance.type == protocol::defs::kSystemTypeId || instance.instance == 255)
        {
            invalid("invalid layout instance");
        }
        const auto type = snapshot.types.find(instance.type);
        if (type == snapshot.types.end()) { invalid("instance type is not described"); }
        ModelInstance entry{instance, params(client, type->second, instance.instance, pending, batch_size)};
        if (!result.emplace(InstanceKey{instance.type, instance.instance}, std::move(entry)).second)
        {
            invalid("duplicate layout instance");
        }
    }
    return result;
}

DeviceSnapshot load(Client &client)
{
    DeviceSnapshot snapshot;
    snapshot.info = client.info(); // validates version and negotiates payload
    // GET request: four-byte core header, count, then four bytes/ref. The reply
    // has a six-byte header and count. Bound both by the negotiated cap.
    if (snapshot.info.max_payload < 11) { invalid("payload cap cannot fit one parameter"); }
    const size_t batch_size = std::min<size_t>(64, (snapshot.info.max_payload - 7) / 4);
    for (const auto &descriptor : client.types())
    {
        ModelType type;
        type.descriptor = descriptor;
        for (const auto &item : client.items(descriptor.type))
        {
            ModelItem entry{item, {}};
            if (item.value_type == ValueType::Enum)
            {
                entry.enums = client.enums(descriptor.type, item.id);
                std::set<uint8_t> seen;
                for (const auto &e : entry.enums)
                {
                    if (!seen.insert(e.value).second) { invalid("duplicate enum value"); }
                }
            }
            if (!type.items.emplace(item.id, std::move(entry)).second) { invalid("duplicate item ID"); }
        }
        if (!snapshot.types.emplace(descriptor.type, std::move(type)).second) { invalid("duplicate type ID"); }
    }
    snapshot.codes = client.codes();
    const auto system = snapshot.types.find(protocol::defs::kSystemTypeId);
    if (system == snapshot.types.end()) { invalid("system type is not described"); }
    const auto configured = client.instances(false);
    const auto pending = client.instances(true);
    snapshot.configured = layout(client, snapshot, configured, false, batch_size);
    snapshot.pending = layout(client, snapshot, pending, true, batch_size);
    snapshot.system_params = params(client, system->second, 0, false, batch_size);
    return snapshot;
}
} // namespace

DeviceModel::DeviceModel(Client &client) : m_client(client), m_snapshot(load(client)) {}

void DeviceModel::reload()
{
    auto next = load(m_client);
    using std::swap;
    swap(m_snapshot, next);
}
} // namespace drivelink
