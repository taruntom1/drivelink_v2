#pragma once

#include "drivelink/client.hpp"
#include <map>
#include <utility>

namespace drivelink
{
struct ModelItem
{
    ItemDescriptor descriptor;
    std::vector<EnumEntry> enums;
};
struct ModelType
{
    TypeDescriptor descriptor;
    std::map<uint16_t, ModelItem> items;
};
struct ModelInstance
{
    Instance instance;
    std::map<uint16_t, Value> params;
};
using InstanceKey = std::pair<uint8_t, uint8_t>; // type, instance ID
using ModelLayout = std::map<InstanceKey, ModelInstance>;

// All metadata comes from the device. Signals/inputs/actions have descriptors,
// but only parameters have values here. The system is not a layout entry.
struct DeviceSnapshot
{
    DeviceInfo info;
    std::map<uint8_t, ModelType> types;
    std::vector<CodeDescriptor> codes;
    std::map<uint16_t, Value> system_params;
    ModelLayout configured;
    ModelLayout pending;

    bool safeMode() const { return (info.flags & 8) != 0; }
};

// Single-caller, like Client. Construction loads the model. Keep Client and
// its Session alive; never call from the I/O thread. Reads span multiple device
// requests, not one atomic device transaction. Does not consume event frames.
class DeviceModel
{
  public:
    explicit DeviceModel(Client &client);
    const DeviceSnapshot &snapshot() const { return m_snapshot; }
    // Only a fully loaded replacement is published. Errors preserve the old
    // snapshot. Successful reload invalidates references into the old contents;
    // callers may retain an independent value copy. No automatic refresh.
    void reload();

  private:
    Client &m_client;
    DeviceSnapshot m_snapshot;
};
} // namespace drivelink
