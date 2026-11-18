// The Tune page's device side (H2e-1): live gain writes and the run.
//
// A run (one at a time, only with this window's accepted ARM and its command
// groups ready): the controller's manual commands are suspended at 0 as for a
// live write (CommandView::beginWrite), then one chain on the service
// executor configures the capture with the test-signal-start trigger, arms it
// and starts the generator. A refused START aborts the capture; a START
// without a reply is followed by STOP and ABORT (it may have started). The
// end of the signal (TestgenDone, or a STATUS poll) ends the suspension: the
// value group sends a fresh frame, so a commandable target returns to 0 (user
// decision 2026-11-18). A run given up while its signal may still run sends
// STOP once and keeps the suspension until STATUS shows the signal stopped:
// a zero frame that lands before STOP is discarded by the generator, which
// then holds its last value. The capture keeps recording after Stop or a
// disarm; when it is Done its data is read once and the run joins the list.
// Boot, a lost link and close end the run; nothing is retried or re-armed.
//
// H2e-2 (user decisions 2026-11-18): with Auto re-run on, a gain change during
// a run cancels it: STOP and ABORT once, the run is not kept, and the
// controller stays at 0 until STATUS shows the signal stopped (the same
// release as a given-up run). Auto re-runs start through startTuneRun like
// Run, only under the ARM of the user's last Run (m_armEpoch) and only while
// Tune is shown.
//
// H2e-3 (user decisions 2026-11-18): plant identification runs one open-loop
// step through the same path. The controller is held at 0 from the start; if
// the chosen mode value differs it is written first (a refused write runs
// nothing). When the step ends however it ends (completed, Stop, disarm, a
// failure) and its signal has stopped on the device (the release), the hold
// ends with a fresh frame and then the previous mode is written back (another
// live write, at 0). Boot and close write nothing back (a reboot restores the
// saved mode; a lost link says the mode may still be set).
#include "connection_internal.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <QFileInfo>
#include <QPointer>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
const ModelItem *findTuneItem(const DeviceSnapshot &s,const Ref &r) {
    const auto t=s.types.find(r.type);if(t==s.types.end()) {return nullptr;}
    const auto i=t->second.items.find(r.item);return i==t->second.items.end()?nullptr:&i->second;
}
bool hasFlag(const ItemDescriptor &d,defs::ItemFlag f) {return (d.flags&uint16_t(f))!=0;}
}

void LocalConnection::setupTune() {
    m_tuneTimer.setInterval(100);
    connect(&m_tuneTimer,&QTimer::timeout,this,[this] {
        if(!m_tuneRun && !m_tuneRelease && !m_modeRestore) {m_tuneTimer.stop();return;}
        pollTune();pollTuneRelease();
        if(m_modeRestore && !m_tuneRelease) {restoreMode();}
    });
    m_tuneRefresh.setSingleShot(true);m_tuneRefresh.setInterval(300);
    connect(&m_tuneRefresh,&QTimer::timeout,this,[this] {refreshConfig();});
    connect(&m_tune,&TuneView::notification,this,&Connection::notification);
    TuneView::Hooks hooks;
    hooks.write=[this](std::vector<Ref> refs,std::vector<uint32_t> bits,std::function<void(QString)> done) {
        tuneWrite(std::move(refs),std::move(bits),std::move(done));
    };
    hooks.runBlocked=[this] {return tuneBlocked();};
    hooks.start=[this] {startTuneRun();};
    hooks.stop=[this] {stopTuneRun();};
    hooks.cancel=[this] {cancelTuneRun();};
    hooks.armEpoch=[this]()->uint64_t {return m_ownArm && armState()==1?m_armEpoch:0;};
    hooks.autoBlocked=[this] {return m_page==QLatin1String("tune")?QString():tr("Tune is not shown");};
    m_tune.setHooks(std::move(hooks));
    IdentifyView::Hooks identify;
    identify.runBlocked=[this]()->QString {
        if(m_tuneRun) {return tr("A run is in progress");}
        if(m_modeRestore) {return tr("The last step's mode is being written back");}
        if(m_tune.writesPending()) {return tr("Wait for the gain and mode writes to finish");}
        return tuneBlocked();
    };
    identify.start=[this] {startIdentify();};
    identify.stop=[this] {stopTuneRun();};
    identify.armEpoch=[this]()->uint64_t {return m_ownArm && armState()==1?m_armEpoch:0;};
    identify.apply=[this](const ResolvedGains &set,const QString &note) {return m_tune.applyGains(set,note);};
    identify.applyBlocked=[this] {return m_tune.applyBlocked();};
    m_tune.identifyView().setHooks(std::move(identify));
}

void LocalConnection::runPhase(TuneView::Phase phase,const QString &text,double progress) {
    if(m_tuneRun && m_tuneRun->identify) {
        using P=IdentifyView::Phase;
        const P p=phase==TuneView::Phase::Starting?P::Starting:phase==TuneView::Phase::Running?P::Running:
                  phase==TuneView::Phase::Capturing?P::Capturing:phase==TuneView::Phase::Reading?P::Reading:P::Idle;
        m_tune.identifyView().setPhase(p,text,progress);
        return;
    }
    m_tune.setPhase(phase,text,progress);
}
QString LocalConnection::runLabel() const {
    if(m_tuneRun && m_tuneRun->identify) {return tr("The step");}
    return tr("Run %1").arg(m_tune.run()["next"].toInt());
}

QString LocalConnection::tuneBlocked() const {
    if(const auto common=commonBlock();!common.isEmpty()) {return common;}
    if(m_expectReboot) {return tr("The device is rebooting");}
    if(m_tuneRelease) {return tr("The last run's signal has not shown it stopped yet; DISARM stops it");}
    // G5: Run is disabled while disarmed, pointing to ARM in the status bar.
    if(armState()==0 && !m_arming && !m_armPending && !m_waitingDisarm && !m_disarmPending) {
        return tr("Arm first: ARM is in the status bar");
    }
    if(const auto why=commandBlocked();!why.isEmpty()) {return why;}
    return {};
}

void LocalConnection::syncTune() {
    // Autosaved sessions live beside the GUI's settings, one per device id.
    m_tune.setRecoveryFolder(QFileInfo(m_recent.path()).absolutePath()+QStringLiteral("/tune-recovery"));
    if(ready()) {m_tune.setSnapshot(m_snapshot,deviceId());}
    // The fast path for the end of the signal: the TestgenDone event.
    if(m_tuneRun && m_tuneRun->run && !m_tuneRun->signalEnded && m_snapshot &&
       m_snapshot->generation==m_tuneRun->generation) {
        const auto it=m_snapshot->testgen.find(m_tuneRun->loop);
        if(it!=m_snapshot->testgen.end() && it->second.run==*m_tuneRun->run) {
            tuneSignalEnded(it->second.reason,std::nullopt);
        }
    }
    m_tune.refreshRunState();
}

void LocalConnection::tuneWrite(std::vector<Ref> refs,std::vector<uint32_t> bits,std::function<void(QString)> done) {
    tuneWriteEx(std::move(refs),std::move(bits),[done=std::move(done)](QString error,bool) {done(error);});
}
void LocalConnection::tuneWriteEx(std::vector<Ref> refs,std::vector<uint32_t> bits,std::function<void(QString,bool)> done) {
    auto fail=[&](const QString &why) {done(why,false);};
    if(refs.empty() || refs.size()!=bits.size() || !m_snapshot) {return fail(tr("Not connected"));}
    QString blocked=commonBlock();
    if(blocked.isEmpty() && m_expectReboot) {blocked=tr("The device is rebooting");}
    const InstanceKey instance{refs.front().type,refs.front().instance};
    for(const auto &ref:refs) {
        const auto *item=findTuneItem(*m_snapshot,ref);
        if(blocked.isEmpty() && (!item || item->descriptor.kind!=ItemKind::Param || InstanceKey{ref.type,ref.instance}!=instance)) {
            blocked=tr("Not a param of this instance");
        }
        if(!blocked.isEmpty()) {break;}
        const auto &d=item->descriptor;
        // Tune writes live values only: never a staged (reboot) param.
        if(hasFlag(d,defs::ItemFlag::RebootRequired) || hasFlag(d,defs::ItemFlag::ReadOnly)) {blocked=tr("Not a live param");}
        else if((armState()==1 || m_arming) && hasFlag(d,defs::ItemFlag::LockedWhileArmed)) {blocked=tr("Locked while armed; disarm first");}
    }
    if(!blocked.isEmpty()) {return fail(blocked);}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return fail(tr("Not connected"));}
    const auto id=deviceId();const auto generation=m_snapshot->generation;const auto snapshot=m_snapshot;
    // The same path as Layout's live writes (user decisions 2026-11-18): an
    // instance this GUI commands goes to 0 for the write, its queued values
    // discarded first, and its group sends a fresh frame when it ends.
    const bool commanded=m_command.commands(instance.first,instance.second);
    if(commanded) {discardCommands(instance.first,instance.second);m_command.beginWrite(instance.first,instance.second);}
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([refs,bits](Client &client) {return client.set(refs,bits);},
            [self,c,refs,bits,id,generation,snapshot,commanded,instance,done](std::expected<ParamResult,Error> result) {
            if(!self || !self->current(c)) {return;}
            const bool sameBoot=self->m_snapshot && self->m_snapshot->generation==generation;
            if(commanded && sameBoot) {self->m_command.endWrite(instance.first,instance.second);}
            if(!result) {
                const auto &e=result.error();
                if(e.outcome_unknown) {self->refreshConfig();}
                done(e.outcome_unknown?tr("No reply in time; the device may have applied it"):layoutRefusal(*snapshot,e,refs.front(),bits.front()),
                     e.outcome_unknown);
                return;
            }
            if(sameBoot) {
                // One change line per param: a drag replaces its own line.
                const auto *entry=anyEntry(*snapshot,instance);
                for(size_t i=0;i<refs.size();++i) {
                    const auto *item=findTuneItem(*snapshot,refs[i]);
                    if(!entry || !item) {continue;}
                    const auto key=itemKey(refs[i].type,refs[i].instance,refs[i].item);
                    const auto applied=i<result->values.size()?result->values[i]:bits[i];
                    const auto subject=QString::fromStdString(entry->instance.label)+"."+QString::fromStdString(item->descriptor.name);
                    auto &lines=self->m_changes[id];
                    auto &mine=self->m_tuneLines[id+"/"+key];
                    QString before;
                    const auto at=mine.line.isEmpty()?-1:lines.indexOf(mine.line);
                    if(at>=0) {before=mine.before;lines.removeAt(at);}
                    else if(const auto it=entry->params.find(refs[i].item);it!=entry->params.end()) {before=formatValue(*item,valueBits(it->second));}
                    const auto after=formatValue(*item,applied);
                    if(at>=0 && before==after) {
                        // Back where this line began (a mode written back, a gain undone).
                        // The param stays changed while another line (Layout's) names it.
                        mine={};
                        const auto prefix=subject+": ";
                        if(std::none_of(lines.begin(),lines.end(),[&](const QString &l) {return l.startsWith(prefix);})) {
                            self->m_changedParams[id].remove(key);
                        }
                        continue;
                    }
                    mine={tr("%1: %2 to %3").arg(subject,before,after),before};
                    lines<<mine.line;
                    self->m_changedParams[id].insert(key);
                }
            }
            // A drag writes every 50 ms: the configuration is read back once it pauses.
            self->m_tuneRefresh.start();
            done({},false);
            emit self->changed();
        });
    } catch(const Error &e) {
        if(commanded) {m_command.endWrite(instance.first,instance.second);}
        done(errorText(e),false);
    }
}

void LocalConnection::startTuneRun() {
    if(m_tuneRun) {return;}
    QString problem=tuneBlocked();
    const auto setup=problem.isEmpty()?m_tune.runSetup(&problem):std::nullopt;
    if(!setup) {emit notification(tr("Run: %1").arg(problem),2);return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session || !m_snapshot) {return;}
    const auto key=setup->controller.key;
    TuneRun run;
    run.setup=*setup;run.generation=m_snapshot->generation;run.loop=setup->controller.loop;run.serial=++m_tuneSerial;
    run.elapsed.start();
    // The controller takes no manual command during the run; its value group
    // sends 0 for it until the signal ends (then a fresh frame).
    run.holding=m_command.commands(key.first,key.second);
    if(run.holding) {discardCommands(key.first,key.second);m_command.beginWrite(key.first,key.second);}
    m_tuneRun=std::move(run);
    m_tune.setPhase(TuneView::Phase::Starting,tr("Starting run %1").arg(m_tune.run()["next"].toInt()));
    startTuneChain();
}

void LocalConnection::startIdentify() {
    if(m_tuneRun || m_modeRestore) {return;}
    QString problem=tuneBlocked();
    auto &view=m_tune.identifyView();
    const auto setup=problem.isEmpty()?view.setup(&problem):std::nullopt;
    if(!setup) {emit notification(tr("Plant identification: %1").arg(problem),2);return;}
    if(m_tune.phase()!=TuneView::Phase::Idle) {emit notification(tr("Plant identification: a run is in progress"),2);return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session || !m_snapshot) {return;}
    const auto key=setup->run.controller.key;
    TuneRun run;
    run.setup=setup->run;run.identify=*setup;run.armEpoch=m_armEpoch;
    run.generation=m_snapshot->generation;run.loop=setup->run.controller.loop;run.serial=++m_tuneSerial;
    run.elapsed.start();
    // The controller stays at 0 from now until the mode is written back.
    run.holding=m_command.commands(key.first,key.second);
    if(run.holding) {discardCommands(key.first,key.second);m_command.beginWrite(key.first,key.second);}
    m_tuneRun=std::move(run);
    m_tune.setIdentifying(true);
    const auto &mode=setup->mode;
    if(!mode) {
        view.setPhase(IdentifyView::Phase::Starting,tr("Starting the step"));
        startTuneChain();
        return;
    }
    // The value to write back is read from the device, not the snapshot: a
    // mode write just before may not have been read back yet. The service
    // executor runs requests in order, so the read follows any write sent.
    view.setPhase(IdentifyView::Phase::Switching,tr("Reading %1").arg(mode->label.section(QLatin1Char(':'),0,0)));
    const auto serial=m_tuneRun->serial;
    const Ref ref{key.first,key.second,mode->item};
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([ref](Client &client)->std::optional<uint32_t> {
            const std::array<Ref,1> refs{{ref}};
            const auto read=client.get(refs);
            return read.values.empty()?std::nullopt:std::optional<uint32_t>(read.values.front());
        },[self,c,serial](std::expected<std::optional<uint32_t>,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->serial!=serial) {return;}
            if(!result || !*result) {
                self->endTuneRun(tr("The mode could not be read: %1. Nothing ran.").arg(result?tr("no value"):errorText(result.error())));
                return;
            }
            self->m_tuneRun->identify->previous=**result;
            self->identifySwitchMode();
        });
    } catch(const Error &e) {endTuneRun(tr("The mode could not be read: %1. Nothing ran.").arg(errorText(e)));}
}

void LocalConnection::identifySwitchMode() {
    if(!m_tuneRun || !m_tuneRun->identify || !m_tuneRun->identify->mode) {return;}
    if(m_tuneRun->stopRequested) {endTuneRun(tr("The step was stopped before it started"));return;}
    const auto &setup=*m_tuneRun->identify;
    const auto &mode=*setup.mode;
    auto &view=m_tune.identifyView();
    if(setup.previous && *setup.previous==mode.value) {
        view.setPhase(IdentifyView::Phase::Starting,tr("Starting the step"));
        startTuneChain();
        return;
    }
    view.setPhase(IdentifyView::Phase::Switching,tr("Setting %1").arg(mode.label));
    const auto serial=m_tuneRun->serial;
    const auto key=setup.run.controller.key;
    const Ref ref{key.first,key.second,mode.item};
    tuneWriteEx({ref},{mode.value},[this,serial](QString error,bool uncertain) {
        if(!m_tuneRun || m_tuneRun->serial!=serial) {return;}
        // An unanswered write may have landed: the previous value goes back.
        if(error.isEmpty() || uncertain) {m_tuneRun->modeWritten=true;}
        if(!error.isEmpty()) {endTuneRun(tr("The mode for the step was not set: %1. Nothing ran.").arg(error));return;}
        if(m_tuneRun->stopRequested || m_tuneRun->cancelRequested) {endTuneRun(tr("The step was stopped before it started"));return;}
        m_tune.identifyView().setPhase(IdentifyView::Phase::Starting,tr("Starting the step"));
        startTuneChain();
    });
}

void LocalConnection::startTuneChain() {
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!m_tuneRun) {return;}
    if(!session || !m_snapshot) {endTuneRun(tr("Not connected"));return;}
    // The step confirmed under one ARM never starts under another.
    if(m_tuneRun->identify && (!m_ownArm || armState()!=1 || m_armEpoch!=m_tuneRun->armEpoch)) {
        endTuneRun(tr("The ARM changed before the step started; nothing ran"));return;
    }
    const auto &setup=m_tuneRun->setup;
    const auto loop=setup.controller.loop;
    const auto capture=setup.capture;
    const auto config=testgenConfig(setup.signal,loop);
    const QPointer<LocalConnection> self(this);
    const auto serial=m_tuneRun->serial;
    // One step at a time; a failure after a CONFIG that may have been accepted
    // ABORTs the capture (a refused CONFIG may be another client's capture).
    enum class Step {Config,Arm,Start};
    struct Started {std::optional<TestgenStarted> started;std::optional<Error> error;Step step=Step::Config;bool mayRun=false;};
    try {
        Client(*session).async([loop,capture,config](Client &client) {
            Started out;
            try {
                CaptureTrigger trigger;trigger.kind=1; // testgen_start
                client.captureConfig(loop,capture.divider,capture.samples,capture.pre,trigger,capture.refs);
                out.step=Step::Arm;client.captureArm();
                out.step=Step::Start;out.started=client.testgenStart(config);
                return out;
            } catch(const Error &e) {out.error=e;}
            // No reply to START: it may have started; STOP it (idempotent) and
            // ask STATUS whether a signal still runs.
            if(out.step==Step::Start && out.error->outcome_unknown) {
                try {client.testgenStop(loop);} catch(const Error &) {}
                try {out.mayRun=client.testgenStatus(loop).state==TestgenState::Running;}
                catch(const Error &) {out.mayRun=true;}
            }
            if(out.step!=Step::Config || out.error->outcome_unknown) {
                try {client.captureAbort();} catch(const Error &) {}
            }
            return out;
        },[self,c,serial](std::expected<Started,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->serial!=serial) {return;}
            if(!result || result->error) {
                const auto &e=result?*result->error:result.error();
                const auto step=result?result->step:Step::Config;
                QString why;
                if(!e.outcome_unknown) {
                    why=step==Step::Config?tr("The capture could not be set up: %1"):step==Step::Arm?tr("The capture could not be armed: %1")
                       :tr("The run did not start: %1");
                    why=why.arg(errorText(e));
                } else {
                    why=step==Step::Start?tr("No reply to the run's start; STOP was sent. Nothing was recorded.")
                       :tr("No reply while setting up the capture; it was cleared. Nothing ran.");
                }
                if(result && result->mayRun) {self->holdUntilStopped();}
                self->endTuneRun(why);
                return;
            }
            self->m_tuneRun->run=result->started->run;
            self->runPhase(TuneView::Phase::Running,tr("%1 running").arg(self->runLabel()),0);
            self->m_tuneTimer.start();
            if(self->m_tuneRun->cancelRequested) {self->cancelTuneRun();return;}
            if(self->m_tuneRun->stopRequested) {self->stopTuneRun();}
            self->syncTune(); // a short signal may already have ended
        });
    } catch(const Error &e) {endTuneRun(tr("The run did not start: %1").arg(errorText(e)));}
}

void LocalConnection::stopTuneRun() {
    if(!m_tuneRun) {return;}
    if(m_tuneRun->identify) {m_tune.identifyView().setStopping(true);} else {m_tune.setStopping(true);}
    if(!m_tuneRun->run) {m_tuneRun->stopRequested=true;return;}
    if(m_tuneRun->signalEnded) {return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    const auto loop=m_tuneRun->loop;
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([loop](Client &client) {return client.testgenStop(loop);},[self,c](auto result) {
            if(!self || !self->current(c)) {return;}
            if(!result) {self->report(result.error());}
            self->pollTune();
        });
    } catch(const Error &e) {report(e);}
}

void LocalConnection::cancelTuneRun() {
    // Plant identification locks the gains: nothing cancels its step.
    if(!m_tuneRun || m_tuneRun->identify) {return;}
    // START not answered yet: cancelled when it is (the chain must not leave
    // a signal running unseen).
    if(!m_tuneRun->run) {
        m_tuneRun->cancelRequested=true;
        m_tune.setPhase(TuneView::Phase::Starting,tr("Cancelling: a gain changed"));
        return;
    }
    const int number=m_tune.run()["next"].toInt();
    // STOP while the signal may run (its release keeps the controller at 0
    // until STATUS shows it stopped), ABORT the capture; nothing is kept.
    m_tuneRun->quietHold=true;
    endTuneRun(tr("Run %1 cancelled: a gain changed").arg(number),TuneEnd::AbortCapture,0);
}

void LocalConnection::tuneSignalEnded(TestgenReason reason,std::optional<uint32_t> elapsedMs) {
    if(!m_tuneRun) {return;}
    auto &run=*m_tuneRun;
    if(elapsedMs && reason!=TestgenReason::Completed) {run.endedUs=int64_t(*elapsedMs)*1000;}
    if(run.signalEnded) {return;}
    run.signalEnded=true;
    run.end=reason==TestgenReason::Stopped?RunEnd::Stopped:reason==TestgenReason::Disarmed?RunEnd::Disarmed:RunEnd::Completed;
    const bool identify=run.identify.has_value();
    if(run.holding) {
        const auto key=run.setup.controller.key;
        if(identify) {
            // Identification keeps the controller held until its mode is
            // written back; a fresh frame returns the step input to 0 now.
            m_command.zeroInstance(key.first,key.second);
        } else {
            run.holding=false;
            m_command.endWrite(key.first,key.second);
        }
    }
    if(run.end==RunEnd::Disarmed) {emit notification(tr("%1 was cut by a disarm; the capture keeps recording").arg(runLabel()),2);}
    runPhase(TuneView::Phase::Capturing,tr("Capture finishing"),-1);
    // The signal has stopped: the mode goes back now, not after the capture.
    if(identify) {queueModeRestore();restoreMode();}
    pollTune();
}

void LocalConnection::pollTune() {
    if(!m_tuneRun || !m_tuneRun->run || m_tuneRun->polling || m_tuneRun->reading) {return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    auto &run=*m_tuneRun;
    // Beyond the signal and the capture, something is wrong: give up.
    const double rate=double(loopRateHz(*m_snapshot,run.loop));
    const double captureSeconds=rate>0?double(run.setup.capture.samples)*run.setup.capture.divider/rate:0;
    if(run.elapsed.elapsed()>qint64((signalSeconds(run.setup.signal)+captureSeconds)*1000)+10000) {
        endTuneRun(tr("The run did not finish in time; nothing was recorded"),TuneEnd::AbortCapture);return;
    }
    run.polling=true;
    const auto loop=run.loop;const auto serial=run.serial;
    const QPointer<LocalConnection> self(this);
    struct Poll {CaptureStatus capture;TestgenStatus testgen;};
    try {
        Client(*session).async([loop](Client &client) {return Poll{client.captureStatus(),client.testgenStatus(loop)};},
            [self,c,serial](std::expected<Poll,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->serial!=serial) {return;}
            auto &run=*self->m_tuneRun;
            run.polling=false;
            if(!result) {return;} // the next tick asks again (reads are idempotent)
            const auto &t=result->testgen;
            if(t.run==*run.run && t.state==TestgenState::Done) {self->tuneSignalEnded(t.reason,t.elapsed_ms);}
            else if(!run.signalEnded && t.run!=*run.run) {
                // Another run replaced ours: it has ended.
                self->tuneSignalEnded(TestgenReason::Stopped,std::nullopt);
            } else if(!run.signalEnded) {
                const double total=signalSeconds(run.setup.signal)*1000;
                self->runPhase(TuneView::Phase::Running,tr("%1 · %2 s of %3 s").arg(self->runLabel(),
                    QString::number(t.elapsed_ms/1000.0,'f',1),QString::number(total/1000,'f',1)),total>0?std::min(1.0,t.elapsed_ms/total):-1);
            }
            const auto &cap=result->capture;
            if(cap.state==CaptureState::Idle || !cap.configured) {
                self->endTuneRun(tr("The capture was cleared (another client?); nothing was recorded"));return;
            }
            if(cap.state==CaptureState::Done && !run.data) {self->readTuneCapture();}
            else if(run.signalEnded && !run.data) {
                self->runPhase(TuneView::Phase::Capturing,tr("Capture %1 of %2 samples").arg(cap.recorded).arg(cap.samples),
                    cap.samples?double(cap.recorded)/cap.samples:-1);
            }
            if(run.data && run.signalEnded) {self->finishTuneRun();}
        });
    } catch(const Error &e) {run.polling=false;endTuneRun(errorText(e));}
}

void LocalConnection::readTuneCapture() {
    if(!m_tuneRun || m_tuneRun->reading) {return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    m_tuneRun->reading=true;
    runPhase(TuneView::Phase::Reading,tr("Reading the capture"),-1);
    const auto serial=m_tuneRun->serial;
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([](Client &client) {return client.captureData();},[self,c,serial](std::expected<CaptureData,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->serial!=serial) {return;}
            self->m_tuneRun->reading=false;
            if(!result) {self->endTuneRun(tr("The capture could not be read: %1").arg(errorText(result.error())));return;}
            self->m_tuneRun->data=std::move(*result);
            if(self->m_tuneRun->signalEnded) {self->finishTuneRun();}
            else {self->runPhase(TuneView::Phase::Running,tr("Capture done; waiting for the signal to end"),-1);}
        });
    } catch(const Error &e) {m_tuneRun->reading=false;endTuneRun(errorText(e));}
}

void LocalConnection::finishTuneRun() {
    if(!m_tuneRun || !m_tuneRun->data || !m_snapshot) {return;}
    auto record=makeRun(*m_snapshot,*m_tuneRun->data,m_tuneRun->setup.capture,m_tuneRun->setup.signal);
    record.end=m_tuneRun->end;record.endedUs=m_tuneRun->endedUs;
    const auto setup=m_tuneRun->setup;
    const auto identify=m_tuneRun->identify;
    endTuneRun({});
    if(identify) {
        record.controller=setup.controller.key;
        m_tune.identifyView().addResult(m_snapshot,std::move(record),*identify);
    } else {
        m_tune.addRun(std::move(record),setup);
    }
}

void LocalConnection::endTuneRun(const QString &failure,TuneEnd how,int tone) {
    const bool sameBoot=how!=TuneEnd::DeviceGone && m_tuneRun && m_snapshot && m_snapshot->generation==m_tuneRun->generation;
    std::optional<InstanceKey> identifyHold;
    if(sameBoot) {
        // Given up while the signal may still run (a timeout, a cleared or
        // unreadable capture): STOP it once (idempotent); a timeout also
        // ABORTs this run's capture. The hold then waits for STATUS.
        const bool stop=m_tuneRun->run && !m_tuneRun->signalEnded;
        const bool abort=how==TuneEnd::AbortCapture;
        auto c=m_context;auto session=c?c->getSession():nullptr;
        if(session && (stop || abort)) {
            const auto loop=m_tuneRun->loop;
            try {
                Client(*session).async([loop,stop,abort](Client &client) {
                    if(stop) {try {client.testgenStop(loop);} catch(const Error &) {}}
                    if(abort) {try {client.captureAbort();} catch(const Error &) {}}
                    return true;
                },[](auto) {});
            } catch(const Error &) {}
        }
        if(stop) {holdUntilStopped(m_tuneRun->quietHold);}
        if(m_tuneRun->holding) {
            const auto key=m_tuneRun->setup.controller.key;
            if(m_tuneRun->identify) {identifyHold=key;}
            else {m_command.endWrite(key.first,key.second);}
        }
    }
    const bool identify=m_tuneRun && m_tuneRun->identify;
    if(identify && sameBoot) {queueModeRestore();}
    if(identifyHold) {
        // The hold moves to a pending write-back; with none it ends here.
        if(m_modeRestore) {m_modeRestore->holding=true;}
        else {m_command.endWrite(identifyHold->first,identifyHold->second);}
    }
    m_tuneRun.reset();
    if(!m_tuneRelease && !m_modeRestore) {m_tuneTimer.stop();}
    if(identify) {
        // Boot and close write nothing back (the snapshot may not show the Boot yet).
        if(!sameBoot) {dropModeRestore();}
        if(m_modeRestore) {restoreMode();} else {endIdentify();}
    } else {
        m_tune.setPhase(TuneView::Phase::Idle);
    }
    if(!failure.isEmpty()) {emit notification(failure,tone);}
}

void LocalConnection::queueModeRestore() {
    if(!m_tuneRun || !m_tuneRun->identify || m_tuneRun->restoreQueued || !m_snapshot) {return;}
    const auto &setup=*m_tuneRun->identify;
    if(!m_tuneRun->modeWritten || !setup.mode || !setup.previous) {return;}
    m_tuneRun->restoreQueued=true;
    const auto key=setup.run.controller.key;
    QString text=setup.mode->label;
    for(const auto &choice:modeChoices(*m_snapshot,key)) {
        if(choice.item==setup.mode->item && choice.value==*setup.previous) {text=choice.label;}
    }
    m_modeRestore=ModeRestore{{key.first,key.second,setup.mode->item},*setup.previous,text,m_tuneRun->generation};
}

void LocalConnection::restoreMode() {
    if(!m_modeRestore || m_modeRestore->sending) {return;}
    if(!m_snapshot || m_snapshot->generation!=m_modeRestore->generation) {dropModeRestore();return;}
    auto &view=m_tune.identifyView();
    // While the capture finishes its phase shows; the write-back's otherwise.
    const bool show=!m_tuneRun;
    // After the signal has stopped (and the hold's fresh frame), never before.
    if(m_tuneRelease) {
        if(show) {view.setPhase(IdentifyView::Phase::Restoring,tr("Waiting for the signal to stop, then %1").arg(m_modeRestore->text));}
        return;
    }
    // A local block (a configuration action, a reboot) is waited out, never
    // taken as a refusal: the timer tries again.
    QString blocked=commonBlock();
    if(blocked.isEmpty() && m_expectReboot) {blocked=tr("The device is rebooting");}
    if(!blocked.isEmpty()) {
        if(show) {view.setPhase(IdentifyView::Phase::Restoring,tr("%1 waits: %2").arg(m_modeRestore->text,blocked));}
        m_tuneTimer.start();
        return;
    }
    m_modeRestore->sending=true;
    const auto restore=*m_modeRestore;
    if(show) {view.setPhase(IdentifyView::Phase::Restoring,tr("Writing back %1").arg(restore.text));}
    tuneWriteEx({restore.ref},{restore.value},[this,restore](QString error,bool) {
        if(!m_modeRestore || m_modeRestore->generation!=restore.generation) {return;}
        const bool holding=m_modeRestore->holding;
        m_modeRestore.reset();
        if(holding) {m_command.endWrite(restore.ref.type,restore.ref.instance);}
        if(!error.isEmpty()) {
            emit notification(tr("%1 was not written back after the step: %2. Set it by hand.").arg(restore.text,error),2);
        }
        if(!m_tuneRun) {endIdentify();}
    });
}

void LocalConnection::endIdentify() {
    m_tune.identifyView().setPhase(IdentifyView::Phase::Idle);
    m_tune.setIdentifying(false);
}

void LocalConnection::dropModeRestore() {
    if(!m_modeRestore) {return;}
    const auto restore=*m_modeRestore;
    m_modeRestore.reset();
    if(restore.holding) {m_command.endWrite(restore.ref.type,restore.ref.instance);}
    if(!m_tuneRun) {endIdentify();}
}

void LocalConnection::holdUntilStopped(bool quiet) {
    if(!m_tuneRun || !m_snapshot) {return;}
    auto &run=*m_tuneRun;
    if(run.holding) {
        // The run's hold moves to the release: endTuneRun must not end it.
        run.holding=false;
        m_tuneRelease=TuneRelease{run.setup.controller.key,run.generation,run.loop,run.run};
    } else {
        // Nothing of ours is held, but a run must not start over a running signal.
        m_tuneRelease=TuneRelease{std::nullopt,run.generation,run.loop,run.run};
    }
    m_tuneTimer.start();
    if(!quiet) {
        emit notification(tr("The test signal may still be running: its target stays at 0 here until the device shows it stopped (DISARM stops it)"),2);
    }
}

void LocalConnection::pollTuneRelease() {
    if(!m_tuneRelease || m_tuneRelease->polling) {return;}
    if(!m_snapshot || m_snapshot->generation!=m_tuneRelease->generation) {m_tuneRelease.reset();dropModeRestore();return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    m_tuneRelease->polling=true;
    const auto loop=m_tuneRelease->loop;const auto generation=m_tuneRelease->generation;
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([loop](Client &client) {return client.testgenStatus(loop);},
            [self,c,generation](std::expected<TestgenStatus,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRelease || self->m_tuneRelease->generation!=generation) {return;}
            auto &release=*self->m_tuneRelease;
            release.polling=false;
            // No reply: the next tick asks again (STATUS is a read). A definite
            // refusal (the loop stopped or was removed) means no generator runs
            // there, so nothing holds a value: the hold ends.
            if(!result && result.error().outcome_unknown) {return;}
            const bool running=result && result->state==TestgenState::Running && (!release.run || result->run==*release.run);
            if(running) {return;}
            if(release.key) {
                if(self->m_modeRestore && !self->m_modeRestore->holding) {
                    // The write-back takes the hold over; the input returns to 0 now.
                    self->m_command.zeroInstance(release.key->first,release.key->second);
                    self->m_modeRestore->holding=true;
                } else {
                    self->m_command.endWrite(release.key->first,release.key->second);
                }
            }
            self->m_tuneRelease.reset();
            if(!self->m_tuneRun) {self->m_tuneTimer.stop();}
            if(self->m_modeRestore) {self->restoreMode();}
            self->m_tune.refreshRunState();
        });
    } catch(const Error &) {m_tuneRelease->polling=false;}
}
} // namespace drivelink::gui
