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
#include "connection_internal.hpp"
#include <drivelink/protocol/gen/registry.hpp>
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
        if(!m_tuneRun && !m_tuneRelease) {m_tuneTimer.stop();return;}
        pollTune();pollTuneRelease();
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
    m_tune.setHooks(std::move(hooks));
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
    auto fail=[&](const QString &why) {done(why);};
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
                done(e.outcome_unknown?tr("No reply in time; the device may have applied it"):layoutRefusal(*snapshot,e,refs.front(),bits.front()));
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
                    mine={tr("%1: %2 to %3").arg(subject,before,formatValue(*item,applied)),before};
                    lines<<mine.line;
                    self->m_changedParams[id].insert(key);
                }
            }
            // A drag writes every 50 ms: the configuration is read back once it pauses.
            self->m_tuneRefresh.start();
            done({});
            emit self->changed();
        });
    } catch(const Error &e) {
        if(commanded) {m_command.endWrite(instance.first,instance.second);}
        done(errorText(e));
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
    run.setup=*setup;run.generation=m_snapshot->generation;run.loop=setup->controller.loop;
    run.elapsed.start();
    // The controller takes no manual command during the run; its value group
    // sends 0 for it until the signal ends (then a fresh frame).
    run.holding=m_command.commands(key.first,key.second);
    if(run.holding) {discardCommands(key.first,key.second);m_command.beginWrite(key.first,key.second);}
    m_tuneRun=std::move(run);
    m_tune.setPhase(TuneView::Phase::Starting,tr("Starting run %1").arg(m_tune.run()["next"].toInt()));
    const auto loop=setup->controller.loop;
    const auto capture=setup->capture;
    const auto config=testgenConfig(setup->signal,loop);
    const QPointer<LocalConnection> self(this);
    const auto generation=m_snapshot->generation;
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
        },[self,c,generation](std::expected<Started,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->generation!=generation) {return;}
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
            self->m_tune.setPhase(TuneView::Phase::Running,tr("Run %1 running").arg(self->m_tune.run()["next"].toInt()),0);
            self->m_tuneTimer.start();
            if(self->m_tuneRun->stopRequested) {self->stopTuneRun();}
            self->syncTune(); // a short signal may already have ended
        });
    } catch(const Error &e) {endTuneRun(tr("The run did not start: %1").arg(errorText(e)));}
}

void LocalConnection::stopTuneRun() {
    if(!m_tuneRun) {return;}
    m_tune.setStopping(true);
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

void LocalConnection::tuneSignalEnded(TestgenReason reason,std::optional<uint32_t> elapsedMs) {
    if(!m_tuneRun) {return;}
    auto &run=*m_tuneRun;
    if(elapsedMs && reason!=TestgenReason::Completed) {run.endedUs=int64_t(*elapsedMs)*1000;}
    if(run.signalEnded) {return;}
    run.signalEnded=true;
    run.end=reason==TestgenReason::Stopped?RunEnd::Stopped:reason==TestgenReason::Disarmed?RunEnd::Disarmed:RunEnd::Completed;
    if(run.holding) {
        run.holding=false;
        const auto key=run.setup.controller.key;
        m_command.endWrite(key.first,key.second);
    }
    if(run.end==RunEnd::Disarmed) {emit notification(tr("The run was cut by a disarm; the capture keeps recording"),2);}
    m_tune.setPhase(TuneView::Phase::Capturing,tr("Capture finishing"),-1);
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
    const auto loop=run.loop;const auto generation=run.generation;
    const QPointer<LocalConnection> self(this);
    struct Poll {CaptureStatus capture;TestgenStatus testgen;};
    try {
        Client(*session).async([loop](Client &client) {return Poll{client.captureStatus(),client.testgenStatus(loop)};},
            [self,c,generation](std::expected<Poll,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->generation!=generation) {return;}
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
                self->m_tune.setPhase(TuneView::Phase::Running,tr("Run %1 · %2 s of %3 s").arg(self->m_tune.run()["next"].toInt())
                    .arg(QString::number(t.elapsed_ms/1000.0,'f',1),QString::number(total/1000,'f',1)),total>0?std::min(1.0,t.elapsed_ms/total):-1);
            }
            const auto &cap=result->capture;
            if(cap.state==CaptureState::Idle || !cap.configured) {
                self->endTuneRun(tr("The capture was cleared (another client?); nothing was recorded"));return;
            }
            if(cap.state==CaptureState::Done && !run.data) {self->readTuneCapture();}
            else if(run.signalEnded && !run.data) {
                self->m_tune.setPhase(TuneView::Phase::Capturing,tr("Capture %1 of %2 samples").arg(cap.recorded).arg(cap.samples),
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
    m_tune.setPhase(TuneView::Phase::Reading,tr("Reading the capture"),-1);
    const auto generation=m_tuneRun->generation;
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([](Client &client) {return client.captureData();},[self,c,generation](std::expected<CaptureData,Error> result) {
            if(!self || !self->current(c) || !self->m_tuneRun || self->m_tuneRun->generation!=generation) {return;}
            self->m_tuneRun->reading=false;
            if(!result) {self->endTuneRun(tr("The capture could not be read: %1").arg(errorText(result.error())));return;}
            self->m_tuneRun->data=std::move(*result);
            if(self->m_tuneRun->signalEnded) {self->finishTuneRun();}
            else {self->m_tune.setPhase(TuneView::Phase::Running,tr("Capture done; waiting for the signal to end"),-1);}
        });
    } catch(const Error &e) {m_tuneRun->reading=false;endTuneRun(errorText(e));}
}

void LocalConnection::finishTuneRun() {
    if(!m_tuneRun || !m_tuneRun->data || !m_snapshot) {return;}
    auto run=std::move(*m_tuneRun);
    auto record=makeRun(*m_snapshot,*run.data,run.setup.capture,run.setup.signal);
    record.end=run.end;record.endedUs=run.endedUs;
    endTuneRun({});
    m_tune.addRun(std::move(record),run.setup);
}

void LocalConnection::endTuneRun(const QString &failure,TuneEnd how) {
    const bool sameBoot=how!=TuneEnd::DeviceGone && m_tuneRun && m_snapshot && m_snapshot->generation==m_tuneRun->generation;
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
        if(stop) {holdUntilStopped();}
        if(m_tuneRun->holding) {
            const auto key=m_tuneRun->setup.controller.key;
            m_command.endWrite(key.first,key.second);
        }
    }
    m_tuneRun.reset();
    if(!m_tuneRelease) {m_tuneTimer.stop();}
    m_tune.setPhase(TuneView::Phase::Idle);
    if(!failure.isEmpty()) {emit notification(failure,2);}
}

void LocalConnection::holdUntilStopped() {
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
    emit notification(tr("The test signal may still be running: its target stays at 0 here until the device shows it stopped (DISARM stops it)"),2);
}

void LocalConnection::pollTuneRelease() {
    if(!m_tuneRelease || m_tuneRelease->polling) {return;}
    if(!m_snapshot || m_snapshot->generation!=m_tuneRelease->generation) {m_tuneRelease.reset();return;}
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
            if(release.key) {self->m_command.endWrite(release.key->first,release.key->second);}
            self->m_tuneRelease.reset();
            if(!self->m_tuneRun) {self->m_tuneTimer.stop();}
            self->m_tune.refreshRunState();
        });
    } catch(const Error &) {m_tuneRelease->polling=false;}
}
} // namespace drivelink::gui
