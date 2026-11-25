#pragma once
// Shared by the LocalConnection sources (connection.cpp, connection_ota.cpp);
// not part of the backend's interface.
#include "connection.hpp"
#include <drivelink/buffers.hpp>
#include <drivelink/command.hpp>
#include <QCoreApplication>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace drivelink::gui {
struct GuiDispatch {
    QObject *receiver=new QObject(qApp);
    std::atomic<size_t> pending{0};
    size_t running=0; // GUI thread only, including nested close-time delivery.
};
namespace detail {
// Each queued functor retains its target until consumed, including after close.
// Session bounds outstanding requests and event deliveries before this queue.
// Safe from libdrivelink's std::thread executors (callbacks and OTA progress on
// services): Qt::QueuedConnection reads only receiver's QThreadData and never
// adopts the calling thread (QThreadData::current(true)), avoiding Windows MinGW
// QAdoptedThread FLS teardown crashes. Keep non-QThread callables free of
// QObject construction and other thread-adopting Qt APIs.
inline void post(const std::shared_ptr<GuiDispatch> &queue,Task task) {
    ++queue->pending;
    QMetaObject::invokeMethod(queue->receiver,[queue,task=std::move(task)]() mutable {
        struct Delivery {
            std::shared_ptr<GuiDispatch> queue;
            ~Delivery() {--queue->running;--queue->pending;}
        } delivery{queue};
        ++queue->running;task();
    },Qt::QueuedConnection);
}
struct Owned {
    CommandPlan plan;uint8_t epoch;uint64_t generation;
    std::shared_ptr<CommandGroup> keeper;
};
inline bool matches(const Owned &o,const Group &g) {
    return g.kind==GroupKind::Command && g.id==o.plan.id && g.configured &&
        g.loop==o.plan.loop && g.epoch==o.epoch && g.refs==o.plan.refs;
}
// A GUI-owned subscription: one loop's statistic signals, (live) the
// Layout selection's signals, or (monitor) one plotted loop of Monitor.
struct OwnedSub {
    uint8_t id,loop,epoch;uint64_t generation;std::vector<Ref> refs;
    std::shared_ptr<GroupBuffer> buffer;
    bool live=false;
    bool monitor=false;
    uint16_t divider=1;
    bool statistics() const {return !live && !monitor;}
};
inline bool matchesSub(const OwnedSub &o,const Group &g) {
    return g.kind==GroupKind::Subscription && g.id==o.id && g.configured &&
        g.loop==o.loop && g.epoch==o.epoch && g.refs==o.refs;
}
} // namespace detail
using namespace detail;
struct LocalConnection::Context {
    std::mutex mutex;
    std::shared_ptr<Session> session;
    std::shared_ptr<DeviceModel> model;
    std::vector<Owned> owned;
    std::vector<OwnedSub> subs;
    std::atomic<bool> retired{false};
    std::atomic<int> bootReason{-1};
    // Set by the service executor once SAVE of Save and reboot succeeded,
    // so a link drop before the reply callback still knows it.
    std::atomic<bool> savedBeforeReboot{false};
    std::atomic<uint64_t> safety{0};
    std::atomic<uint64_t> armedSeen{0};
    // Remote mode: bumps whenever this window gains or loses the lease, so
    // a command setup or ARM started under another lease is recognised.
    std::atomic<uint64_t> leaseEpoch{0};
    std::shared_ptr<Session> getSession() {std::lock_guard lock(mutex);return session;}
    std::shared_ptr<DeviceModel> getModel() {std::lock_guard lock(mutex);return model;}
    bool owns() {std::lock_guard lock(mutex);return !owned.empty();}
    bool ownsAny() {std::lock_guard lock(mutex);return !owned.empty() || !subs.empty();}
    // SUB_CONFIG landed on a newer boot than its setup was for, so nothing
    // tracks the slot: clear it on an exact LIST match, or record it under
    // that boot so the next setup or close reconciles it.
    void releaseStray(Client &client,const OwnedSub &o) {
        try {
            const auto list=client.groups();
            if(std::any_of(list.groups.begin(),list.groups.end(),[&](const auto &g){return matchesSub(o,g);})) {client.groupClear(o.id,false);}
            return;
        } catch(const Error &) {}
        std::lock_guard lock(mutex);subs.push_back(o);
    }
    std::vector<OwnedSub> liveSubs(uint64_t generation) {
        std::lock_guard lock(mutex);std::vector<OwnedSub> result;
        for(const auto &o:subs) {if(o.live && o.generation==generation) {result.push_back(o);}}
        return result;
    }
    std::vector<OwnedSub> monitorSubs(uint64_t generation) {
        std::lock_guard lock(mutex);std::vector<OwnedSub> result;
        for(const auto &o:subs) {if(o.monitor && o.generation==generation) {result.push_back(o);}}
        return result;
    }
    void stopKeepers() {std::lock_guard lock(mutex);for(auto &o:owned) {if(o.keeper) {o.keeper->stop();}}}
    // Remote mode, the lease ended: the agent cleared this window's command
    // groups, but a CMD_CONFIG in flight may still land under a lease held
    // again by then. The records wait for the next setup, which clears an
    // exact match first (clearReleased). Guarded by mutex.
    std::vector<Owned> released;
    void releaseOwned() {
        stopKeepers();
        std::lock_guard lock(mutex);
        for(auto &o:owned) {o.keeper.reset();released.push_back(std::move(o));}
        owned.clear();
    }
    // Called only on the serial service executor, while this window holds
    // the lease and the device is disarmed (command setup). One attempt; a
    // failed LIST or CLEAR never stops the setup (a leftover group of ours
    // has no keeper, so at worst ARM is refused or the watchdog disarms).
    void clearReleased(Client &client) {
        std::vector<Owned> copy;
        {std::lock_guard lock(mutex);copy.swap(released);}
        if(copy.empty()) {return;}
        try {
            const auto list=client.groups();
            for(const auto &o:copy) {
                if(client.session().generation()!=o.generation) {continue;}
                const auto it=std::find_if(list.groups.begin(),list.groups.end(),[&](const auto &g){return matches(o,g);});
                if(it!=list.groups.end()) {
                    try {client.groupClear(o.plan.id,true);} catch(const Error &) {}
                }
            }
        } catch(const Error &) {}
    }
    // Called only on the serial service executor. Never clears a slot without
    // matching its generation, epoch and complete configuration after a read.
    void clearOwned(Client &client) {
        std::vector<Owned> copy;
        {std::lock_guard lock(mutex);copy=owned;}
        stopKeepers();
        for(const auto &o:copy) {
            if(client.session().generation()!=o.generation) {continue;}
            const auto list=client.groups();
            const auto it=std::find_if(list.groups.begin(),list.groups.end(),[&](const auto &g){return matches(o,g);});
            if(it!=list.groups.end()) {client.groupClear(o.plan.id,true);}
        }
        std::lock_guard lock(mutex);owned.clear();
    }
    // Close-time cleanup of everything this connection configured. The
    // statistics groups are released even when a command clear failed.
    void clearAll(Client &client) {
        std::optional<Error> failure;
        try {clearOwned(client);} catch(const Error &e) {failure=e;}
        clearSubs(client);
        if(failure) {throw *failure;}
    }
    // Same rule for loop-statistics subscriptions: clear only an exact match.
    void clearSubs(Client &client) {
        std::vector<OwnedSub> copy;
        {std::lock_guard lock(mutex);copy=subs;}
        if(copy.empty()) {return;}
        const auto list=client.groups();
        for(const auto &o:copy) {
            if(client.session().generation()!=o.generation) {continue;}
            const auto it=std::find_if(list.groups.begin(),list.groups.end(),[&](const auto &g){return matchesSub(o,g);});
            if(it!=list.groups.end()) {client.groupClear(o.id,false);}
        }
        std::lock_guard lock(mutex);subs.clear();
    }
};
} // namespace drivelink::gui
