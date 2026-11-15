#include "drivelink/model.hpp"
#include "drivelink/buffers.hpp"
#include "drivelink/command.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
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
}
int main(int argc,char **argv) {
    if(argc!=2) {std::cerr<<"usage: async_probe ENDPOINT\n";return 2;}
    try {
        Session session(argv[1]);Client client(session);DeviceModel model(client);
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
        const Ref rate{5,loop,protocol::defs::loop::kRateHz};
        bool rate_changed=false;
        try {
            namespace l=protocol::defs::loop;
            const std::array<Ref,4> refs{{{5,loop,l::kTicks},{5,loop,l::kOverruns},{5,loop,l::kLastRunUs},{5,loop,l::kMaxRunUs}}};
            const auto configured=client.subConfigure(0,loop,1,refs);
            auto buffer=session.subscribeBuffer(0,configured.epoch,4,128);
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
            // Native timers are scheduled by a desktop OS. At 1 kHz its
            // three-period safety threshold is below normal Windows jitter.
            // Exercise arming independently at 50 Hz, then restore 1 kHz.
            if(native) {const uint32_t slow=50;client.set({&rate,1},{&slow,1});rate_changed=true;}
            command=client.managedCommand(0,loop,{},{});client.arm();
            waitFor([&]{auto s=model.snapshotShared();return s->status && s->status->arm_state==1;});
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if(model.snapshotShared()->status->arm_state!=1) {
                const auto status=*model.snapshotShared()->status;
                std::cerr<<"arm="<<unsigned(status.arm_state)<<" reason="<<unsigned(status.last_disarm_reason)
                         <<" managed_active="<<command->active()<<" rejected="<<command->rejected()<<'\n';
                for(const auto &d:client.diagEntries()) {std::cerr<<"diag "<<unsigned(d.source)<<':'<<d.code<<" count="<<d.count<<" state="<<unsigned(d.state)<<'\n';}
                throw Error(ErrorKind::Protocol,"keepalives did not preserve arming");
            }
            command->stop();
            waitFor([&]{auto s=model.snapshotShared();return s->status && s->status->arm_state==0 && s->status->last_disarm_reason==2;},watchdog_ms+5000);
            std::promise<std::expected<SystemResult,Error>> disarmed;auto done=disarmed.get_future();
            client.disarmAsync([&](auto r){disarmed.set_value(std::move(r));});
            require(done.wait_for(std::chrono::seconds(5))==std::future_status::ready && bool(done.get()),"async DISARM failed");
            client.groupClear(0,true);client.groupClear(0,false);
            if(rate_changed) {const uint32_t restore=1000;client.set({&rate,1},{&restore,1});rate_changed=false;}
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
            try {client.disarm();client.groupClear(0,true);client.groupClear(0,false);}catch(...) {}
            if(rate_changed) {try {const uint32_t restore=1000;client.set({&rate,1},{&restore,1});}catch(...) {}}
            throw;
        }
        model.onChange({});session.close();
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
