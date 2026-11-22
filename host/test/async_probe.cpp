#include "drivelink/model.hpp"
#include "drivelink/buffers.hpp"
#include "drivelink/command.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace drivelink;
namespace {
void require(bool condition,const char *message) {if(!condition) {throw Error(ErrorKind::Protocol,message);}}
template<class F> void waitFor(F predicate,unsigned ms=5000) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    while(!predicate()) {require(std::chrono::steady_clock::now()<deadline,"probe condition timed out");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
// Existing bench/vdev policy: only these group rejections guarantee no effect.
// Never retry a mutation timeout, uncertain outcome or another family's Busy.
template<class F> auto groupRetry(F operation) {
    for(unsigned attempt=0;;++attempt) {
        try {return operation();}
        catch(const Error &e) {
            namespace d=protocol::defs;
            if(attempt>=9 || e.outcome_unknown || e.kind!=ErrorKind::Rejected || !e.has_request_context || e.family!=static_cast<uint8_t>(d::Family::Group) ||
               e.result!=static_cast<uint8_t>(d::Result::Busy) ||
               (e.detail!=static_cast<uint8_t>(d::Detail::StagingBusy) && e.detail!=static_cast<uint8_t>(d::Detail::Withdrawn))) {throw;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
}
int main(int argc,char **argv) {
    if(argc!=2) {std::cerr<<"usage: async_probe ENDPOINT\n";return 2;}
    try {
        Session session(argv[1]);Client client(session);
        std::promise<std::expected<std::shared_ptr<DeviceModel>,Error>> connected;auto loading=connected.get_future();
        client.async([](Client &worker){return std::make_shared<DeviceModel>(worker);},[&](auto r){connected.set_value(std::move(r));});
        require(loading.wait_for(std::chrono::seconds(15))==std::future_status::ready,"async model connect timed out");
        auto loaded=loading.get();if(!loaded) {throw loaded.error();}auto &model=**loaded;
        const auto original=model.snapshot();
        require(!original.safeMode() && !(original.info.flags&3),"requires clean, valid normal layout");
        const auto groups=client.groups();for(const auto &g:groups.groups) {require(!g.configured,"requires idle groups");}
        uint8_t loop=255;
        for(const auto &l:original.info.loops) {if(l.rate_hz==1000) {loop=l.id;break;}}
        require(loop!=255,"requires a 1 kHz loop");
        std::atomic<unsigned> changes=0,arm_changes=0;
        model.onChange([&](auto change){++changes;if(change==DeviceModel::Change::ArmState) {++arm_changes;}});
        const Ref watchdog{0,0,protocol::defs::system::kWatchdogMs};
        std::promise<std::expected<ParamResult,Error>> typed;auto result=typed.get_future();
        client.async([watchdog](Client &c){return c.get({&watchdog,1});},[&](auto reply){typed.set_value(std::move(reply));});
        require(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,"typed async call did not finish");
        auto reply=result.get();require(bool(reply),"typed async get failed");const auto watchdog_ms=reply->values.at(0);
        std::shared_ptr<CommandGroup> command;
        const bool native=original.info.firmware_version.ends_with("-vdev");
        std::vector<Ref> timing_refs;
        std::vector<uint32_t> saved_timing,native_timing;
        for(const auto &l:original.info.loops) {
            timing_refs.push_back({5,l.id,protocol::defs::loop::kRateHz});
            saved_timing.push_back(l.rate_hz);native_timing.push_back(10);
        }
        timing_refs.push_back(watchdog);saved_timing.push_back(watchdog_ms);native_timing.push_back(1000);
        bool timing_changed=false;
        const auto restoreTiming=[&] {
            if(timing_changed) {
                client.set(timing_refs,saved_timing);
                require(client.get(timing_refs).values==saved_timing,"native timing was not restored");
                timing_changed=false;
            }
        };
        try {
            namespace l=protocol::defs::loop;
            const std::array<Ref,4> refs{{{5,loop,l::kTicks},{5,loop,l::kOverruns},{5,loop,l::kLastRunUs},{5,loop,l::kMaxRunUs}}};
            const auto configured=groupRetry([&]{return client.subConfigure(0,loop,1,refs);});
            auto buffer=session.subscribeBuffer(0,configured.epoch,4,128,configured.generation);
            uint64_t reads=0,mapped=0,last_time=0;uint32_t last_tick=0;GroupSample sample;
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(std::chrono::steady_clock::now()<end) {
                if(buffer->latest(sample) && sample.device_us!=last_time) {
                    require(sample.count==4 && sample.epoch==configured.epoch,"bad buffered metadata");
                    require(sample.device_us>last_time && sample.values[0]>=last_tick,"backwards group sample");
                    require(sample.values[2]<=sample.values[3],"incoherent loop statistic snapshot");
                    require(sample.generation==session.generation(),"stale group generation");
                    last_time=sample.device_us;last_tick=sample.values[0];++reads;if(sample.mapped) {++mapped;}
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(reads>100 && mapped>100,"insufficient mapped 1 kHz data");
            // Desktop scheduling can pause any loop, including an empty one.
            // The real safety monitor checks them all. Test native arming at
            // 10 Hz with a 1 s command watchdog, then restore every setting.
            // The disarmed data test above and physical-board timing stay intact.
            if(native) {timing_changed=true;client.set(timing_refs,native_timing);}
            command=groupRetry([&]{return client.managedCommand(0,loop,{},{});});client.arm();
            waitFor([&]{auto s=model.snapshotShared();return s->status && s->status->arm_state==1;});
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if(model.snapshotShared()->status->arm_state!=1) {
                const auto status=*model.snapshotShared()->status;
                std::cerr<<"arm="<<unsigned(status.arm_state)<<" reason="<<unsigned(status.last_disarm_reason)
                         <<" managed_active="<<command->active()<<" rejected="<<command->rejected()<<'\n';
                for(const auto &d:client.diagEntries()) {std::cerr<<"diag "<<unsigned(d.source)<<':'<<d.code<<" count="<<d.count<<" state="<<unsigned(d.state)<<'\n';}
                throw Error(ErrorKind::Protocol,"keepalives did not preserve arming");
            }
            client.setName("armed mock");
            require(client.info().name=="armed mock" && client.info().device_id==original.info.device_id,"armed identity change failed");
            model.reload();
            require(model.snapshot().info.name=="armed mock" && model.snapshot().status && model.snapshot().status->arm_state==1,"rename changed arm state or failed model reload");
            client.setName(original.info.name);
            std::cout<<"PASS armed rename, immutable id and model identity refresh\n";
            command->stop();
            waitFor([&]{auto s=model.snapshotShared();return s->status && s->status->arm_state==0 && s->status->last_disarm_reason==2;},(native?1000:watchdog_ms)+5000);
            std::promise<std::expected<SystemResult,Error>> disarmed;auto done=disarmed.get_future();
            client.disarmAsync([&](auto r){disarmed.set_value(std::move(r));});
            require(done.wait_for(std::chrono::seconds(5))==std::future_status::ready && bool(done.get()),"async DISARM failed");
            groupRetry([&]{return client.groupClear(0,true);});groupRetry([&]{return client.groupClear(0,false);});
            restoreTiming();
            if(std::string(argv[1]).starts_with("serial://")) {
                const auto generation=session.generation();client.system(8);
                waitFor([&]{auto s=model.snapshotShared();return session.generation()!=generation && s->generation==session.generation() && !s->reloading;},15000);
                require(!buffer->latest(sample),"Boot retained old group buffer");
                require(model.snapshot().configured.size()==original.configured.size(),"Boot did not restore model");
                std::cout<<"PASS Boot invalidation and automatic complete model reload\n";
            }
            model.reload();require(model.snapshot().configured.size()==original.configured.size(),"layout changed");
            require(changes>0 && arm_changes>=2,"missing model/arm notifications");
            std::cout<<"PASS async typed services, model events, managed keepalives/watchdog and DISARM; 1 kHz latest reads="<<reads
                     <<" mapped="<<mapped<<" ring_dropped="<<buffer->dropped()<<"\n";
        } catch(...) {
            if(command) {command->stop();}
            try {client.disarm();groupRetry([&]{return client.groupClear(0,true);});groupRetry([&]{return client.groupClear(0,false);});}catch(...) {}
            try {client.setName(original.info.name);}catch(...) {}
            try {restoreTiming();}catch(...) {}
            throw;
        }
        model.onChange({});session.close();
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
