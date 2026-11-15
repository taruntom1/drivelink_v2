#include "drivelink/model.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <algorithm>
#include <set>
#include <mutex>

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
    snapshot.diagnostics=client.diagEntries();
    return snapshot;
}
} // namespace
namespace {
void retainRuntime(DeviceSnapshot &next,const DeviceSnapshot &previous) {
    next.status=previous.status;next.capture=previous.capture;next.testgen=previous.testgen;
    next.connected=previous.connected;next.notification_losses=previous.notification_losses;
    next.malformed_events=previous.malformed_events;
    if(next.status) {
        const auto flags=next.status->flags;
        next.info.flags=static_cast<uint8_t>((next.info.flags&~9U)|((flags&4U)?1U:0U)|((flags&32U)?8U:0U));
    }
}
}

struct DeviceModel::State : std::enable_shared_from_this<DeviceModel::State>
{
    mutable std::mutex mutex;
    std::recursive_mutex callback_mutex;
    std::mutex load_mutex;
    std::shared_ptr<const DeviceSnapshot> snapshot;
    Handler handler;
    Session *session;
    bool active=true;
    RequestHandle reload;
    bool diag_pending=false;
    uint64_t diag_revision=0;
    uint64_t reload_generation=0;
    bool reload_pending=false;

    explicit State(Session &s):session(&s) {}
    void changed(Change change) {
        std::lock_guard callbacks(callback_mutex);
        Handler callback;
        {std::lock_guard lock(mutex);if(!active) {return;}callback=handler;}
        if(callback) {callback(change);}
    }
    void reloadBoot(uint64_t generation) {
        RequestHandle previous;
        {std::lock_guard lock(mutex);
         if(!active || (reload_pending && reload_generation==generation)) {return;}
         previous=reload;reload_pending=true;reload_generation=generation;}
        previous.cancel();
        try {
            auto state=shared_from_this();
            auto handle=session->async<DeviceSnapshot>([state,generation] {
                std::lock_guard loading(state->load_mutex);
                Client client(*state->session);auto next=load(client);
                if(state->session->generation()!=generation) {throw Error(ErrorKind::Protocol,"boot changed during model reload");}
                next.generation=generation;return next;
            },[weak=weak_from_this(),generation](std::expected<DeviceSnapshot,Error> result) {
                auto state=weak.lock();if(!state) {return;}
                {std::lock_guard lock(state->mutex);
                 if(!state->active || state->snapshot->generation!=generation || state->session->generation()!=generation) {return;}
                 state->reload_pending=false;
                 if(result) {
                     retainRuntime(*result,*state->snapshot);
                     state->snapshot=std::make_shared<DeviceSnapshot>(std::move(*result));
                 } else {
                     auto next=std::make_shared<DeviceSnapshot>(*state->snapshot);next->reloading=false;state->snapshot=std::move(next);
                 }}
                state->changed(result?Change::Model:Change::ReloadFailed);
            });
            std::lock_guard lock(mutex);reload=std::move(handle);
        } catch(const Error &) {
            {std::lock_guard lock(mutex);if(reload_generation==generation) {reload_pending=false;}}
            changed(Change::ReloadFailed);
        }
    }
    void refreshDiagnostics() {
        uint64_t generation,revision;
        {std::lock_guard lock(mutex);if(!active || diag_pending) {return;}diag_pending=true;generation=snapshot->generation;revision=diag_revision;}
        try {
            session->async<std::vector<DiagEntry>>([session=session] {Client client(*session);return client.diagEntries();},
                [weak=weak_from_this(),generation,revision](auto result) {
                    auto state=weak.lock();if(!state) {return;}bool again=false,published=false;
                    {std::lock_guard lock(state->mutex);state->diag_pending=false;
                     if(!state->active || state->snapshot->generation!=generation) {return;}
                     again=revision!=state->diag_revision;
                     if(result && !again) {auto next=std::make_shared<DeviceSnapshot>(*state->snapshot);next->diagnostics=std::move(*result);state->snapshot=std::move(next);published=true;} }
                    if(published) {state->changed(Change::Diagnostic);}if(again) {state->refreshDiagnostics();}
                });
        } catch(const Error &) {std::lock_guard lock(mutex);diag_pending=false;}
    }
};

DeviceModel::DeviceModel(Client &client) : m_session(client.session()),m_state(std::make_shared<State>(client.session())) {
    reload();
    m_observer=client.session().observe([weak=std::weak_ptr<State>(m_state)](const Frame &frame) {
        auto state=weak.lock();if(!state) {return;}
        bool boot=false,arm=false,loss=false;
        Change change=Change::Model;
        {
            std::lock_guard lock(state->mutex);if(!state->active) {return;}
            auto next=std::make_shared<DeviceSnapshot>(*state->snapshot);
            const auto dropped=state->session->droppedNotifications();loss=dropped!=next->notification_losses;next->notification_losses=dropped;
            if(frame.generation<next->generation) {return;}
            boot=frame.generation!=next->generation;
            try {
                auto event=Client::decodeEvent(frame);
                std::visit([&](const auto &item) {
                    using T=std::decay_t<decltype(item)>;
                    if constexpr(std::is_same_v<T,StatusFrame>) {
                        arm=!next->status || next->status->arm_state!=item.arm_state;
                        next->status=item;change=Change::Status;
                        next->info.flags=static_cast<uint8_t>((next->info.flags&~9U)|((item.flags&4U)?1U:0U)|((item.flags&32U)?8U:0U));
                    } else if constexpr(std::is_same_v<T,CaptureDone>) {next->capture=item;}
                    else if constexpr(std::is_same_v<T,TestgenDone>) {next->testgen[item.loop]=item;}
                    else if constexpr(std::is_same_v<T,BootEvent>) {boot=true;}
                    else if constexpr(std::is_same_v<T,DiagRecord>) {
                        change=Change::Diagnostic;
                        auto it=std::find_if(next->diagnostics.begin(),next->diagnostics.end(),[&](const auto &d) {
                            return d.source==item.source && d.instance==item.instance && d.code==item.code;
                        });
                        if(it==next->diagnostics.end()) {
                            if(next->diagnostics.size()==254) {next->diagnostics.erase(next->diagnostics.begin());}
                            next->diagnostics.push_back({item.source,item.instance,item.code,0,1});
                            it=std::prev(next->diagnostics.end());
                        }
                        it->state|=1;
                        const auto code=std::find_if(next->codes.begin(),next->codes.end(),[&](const auto &c) {
                            return c.source==item.source && c.code==item.code;
                        });
                        if(code!=next->codes.end() && code->kind==2) {it->state|=2;}
                    } else if constexpr(std::is_same_v<T,DiagCleared>) {
                        change=Change::Diagnostic;
                        for(auto &d:next->diagnostics) {
                            if(d.source==item.source && d.instance==item.instance && d.code==item.code) {d.state&=~2U;}
                        }
                    } else if constexpr(std::is_same_v<T,DiagCounters>) {
                        change=Change::Diagnostic;
                        for(const auto &counter:item.entries) {
                            auto it=std::find_if(next->diagnostics.begin(),next->diagnostics.end(),[&](const auto &d) {
                                return d.source==counter.source && d.instance==counter.instance && d.code==counter.code;
                            });
                            if(it==next->diagnostics.end()) {
                                if(next->diagnostics.size()==254) {next->diagnostics.erase(next->diagnostics.begin());}
                                next->diagnostics.push_back({counter.source,counter.instance,counter.code,1,counter.total});
                            } else {it->count=counter.total;it->state|=1;}
                        }
                    }
                },event);
            } catch(const Error &) {++next->malformed_events;}
            if(frame.type==0) {next->connected=false;boot=false;change=Change::Link;}
            if(boot) {next->generation=frame.generation;next->ready=false;next->reloading=true;next->status.reset();next->diagnostics.clear();next->capture.reset();next->testgen.clear();}
            state->snapshot=std::move(next);
        }
        if(change==Change::Diagnostic || loss) {
            {std::lock_guard lock(state->mutex);++state->diag_revision;}
            state->refreshDiagnostics();
        }
        state->changed(change);if(arm) {state->changed(Change::ArmState);}
        bool refresh=false;
        {std::lock_guard lock(state->mutex);refresh=state->active && state->snapshot->reloading;}
        if(refresh) {state->reloadBoot(frame.generation);}

    });
}
DeviceModel::~DeviceModel() {
    m_session.unobserve(m_observer);
    std::lock_guard callbacks(m_state->callback_mutex);
    {std::lock_guard lock(m_state->mutex);m_state->active=false;m_state->handler={};}
    m_state->reload.cancel();
}
std::shared_ptr<const DeviceSnapshot> DeviceModel::snapshotShared() const {
    std::lock_guard lock(m_state->mutex);return m_state->snapshot;
}
void DeviceModel::onChange(Handler handler) {std::lock_guard callbacks(m_state->callback_mutex);std::lock_guard lock(m_state->mutex);m_state->handler=std::move(handler);}
RequestHandle DeviceModel::reloadAsync(Completion<OperationResult> completion) {
    const auto generation=m_session.generation();auto state=m_state;
    return m_session.async<OperationResult>([state,generation] {
        std::lock_guard loading(state->load_mutex);
        Client client(*state->session);auto next=load(client);next.generation=generation;
        if(state->session->generation()!=generation) {throw Error(ErrorKind::Protocol,"boot changed during model reload");}
        {std::lock_guard lock(state->mutex);if(state->active) {
            if(state->snapshot->generation!=generation) {throw Error(ErrorKind::Protocol,"model advanced during reload");}
            retainRuntime(next,*state->snapshot);
            state->snapshot=std::make_shared<DeviceSnapshot>(std::move(next));
        }}
        return OperationResult{};
    },[weak=std::weak_ptr<State>(state),completion=std::move(completion)](auto result) mutable {
        if(auto s=weak.lock()) {s->changed(result?Change::Model:Change::ReloadFailed);}
        if(completion) {completion(std::move(result));}
    });
}

void DeviceModel::reload()
{
    std::lock_guard loading(m_state->load_mutex);
    const auto generation=m_session.generation();
    Client client(m_session);auto next=load(client);next.generation=generation;
    if(m_session.generation()!=generation) {throw Error(ErrorKind::Protocol,"boot changed during model reload");}
    std::lock_guard lock(m_state->mutex);
    if(m_state->snapshot) {
        if(m_state->snapshot->generation!=generation) {throw Error(ErrorKind::Protocol,"model advanced during reload");}
        retainRuntime(next,*m_state->snapshot);
    }
    m_state->snapshot=std::make_shared<DeviceSnapshot>(std::move(next));
}
} // namespace drivelink
