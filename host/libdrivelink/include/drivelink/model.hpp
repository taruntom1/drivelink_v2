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
    uint64_t generation=0;
    bool connected=true, reloading=false;
    std::optional<StatusFrame> status;
    std::vector<DiagEntry> diagnostics;
    std::optional<CaptureDone> capture;
    std::map<uint8_t,TestgenDone> testgen;
    uint64_t malformed_events=0;
    uint64_t notification_losses=0;

    bool safeMode() const { return (info.flags & 8) != 0; }
};

// Single-caller, like Client. Construction loads the model. Keep Client and
// its Session alive; never call from the I/O thread. Reads span multiple device
// requests, not one atomic device transaction. Does not consume event frames.
class DeviceModel
{
  public:
    explicit DeviceModel(Client &client);
    ~DeviceModel();
    DeviceModel(const DeviceModel &)=delete;
    DeviceModel &operator=(const DeviceModel &)=delete;
    DeviceSnapshot snapshot() const {return *snapshotShared();}
    std::shared_ptr<const DeviceSnapshot> snapshotShared() const;
    enum class Change {Model,Status,Diagnostic,ArmState,Link,ReloadFailed};
    using Handler=std::function<void(Change)>;
    void onChange(Handler handler);
    RequestHandle reloadAsync(Completion<OperationResult> completion={});
    // Only a fully loaded replacement is published. Errors preserve the old
    // snapshot. Successful reload invalidates references into the old contents;
    // callers may retain an independent value copy. No automatic refresh.
    void reload();

  private:
    Client &m_client;
    struct State;
    std::shared_ptr<State> m_state;
    uint64_t m_observer=0;
};
} // namespace drivelink
