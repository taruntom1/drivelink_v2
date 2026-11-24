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
    // What SAVE writes: the pending value of a reboot_required param, the live
    // one of any other. Differs from system_params only while a setting is
    // staged or saved but not rebooted, or (saved ones) in safe mode.
    std::map<uint16_t, Value> pending_system_params;
    ModelLayout configured;
    ModelLayout pending;
    uint64_t generation=0;
    bool connected=true, reloading=false, ready=true;
    std::optional<StatusFrame> status;
    std::vector<DiagEntry> diagnostics;
    std::optional<CaptureDone> capture;
    std::map<uint8_t,TestgenDone> testgen;
    uint64_t malformed_events=0;
    uint64_t notification_losses=0;

    bool safeMode() const { return (info.flags & 8) != 0; }
};

// Thread-safe immutable snapshots and dispatched notifications. Keep the
// Session alive; the constructor does not retain Client. Loads span several
// device reads, not an atomic device transaction. The legacy event queue is
// independent. Construction/reload are synchronous; never call them on I/O.
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
    // Re-reads only the configured and pending layouts and every parameter
    // (instances and system) with the descriptors already loaded, after a SET
    // or a layout op. Requires a ready snapshot of the current boot; a Boot in
    // between fails it without publishing. Status and diagnostics are kept.
    RequestHandle refreshConfigAsync(Completion<OperationResult> completion={});
    // Explicit reload preserves complete metadata on error. Retained shared
    // snapshots stay valid. Boot marks ready=false until its automatic reload
    // succeeds; runtime events may keep updating while a reload is in progress.
    void reload();

  private:
    Session &m_session;
    struct State;
    std::shared_ptr<State> m_state;
    uint64_t m_observer=0;
};
} // namespace drivelink
