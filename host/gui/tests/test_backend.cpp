#include "connection.hpp"
#include "layout_model.hpp"
#include "../../../protocol/test/ota_fixture.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include <drivelink/protocol/core_codec.hpp>
#include <drivelink/protocol/framing.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QAbstractItemModelTester>
#include <QFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <algorithm>
#include <cstring>
#include <stdexcept>

using namespace drivelink;
using namespace drivelink::gui;
namespace defs=drivelink::protocol::defs;
namespace {
bool waitFor(const std::function<bool()> &condition,int timeout=8000) {
    QElapsedTimer elapsed;elapsed.start();
    while(!condition() && elapsed.elapsed()<timeout) {QCoreApplication::processEvents();QTest::qWait(10);}
    return condition();
}
// A structurally valid, non-executable app image for vdev with this version
// string in its app descriptor (the fixture's checksum and SHA re-sealed).
QString writeImage(const QTemporaryDir &dir,const QString &name,uint8_t seed,const char *version,size_t bytes=4096) {
    auto image=otaFixture(bytes,seed);
    const size_t data=std::max<size_t>(256,(bytes+3)&~size_t(3)),end=image.size()-32;
    std::fill_n(image.begin()+48,32,uint8_t(0));std::copy_n(version,std::strlen(version),image.begin()+48);
    uint8_t checksum=0xef;for(size_t i=32;i<32+data;++i) {checksum^=image[i];}
    image[end-1]=checksum;
    const auto digest=protocol::sha256(std::span(image).first(end));std::copy(digest.begin(),digest.end(),image.begin()+end);
    QFile file(dir.filePath(name));
    if(!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char *>(image.data()),qint64(image.size()))!=qint64(image.size())) {
        throw std::runtime_error("cannot write image");
    }
    return file.fileName();
}
QString otaText(LocalConnection &c) {
    const auto ota=c.ota();
    return QString("phase %1: %2 / %3").arg(ota["phase"].toString(),ota["note"].toString(),c.lastError()["message"].toString());
}
QQuickItem *visualChild(QQuickItem *parent,const QString &name) {
    if(parent->objectName()==name) {return parent;}
    for(auto *child:parent->childItems()) {if(auto *found=visualChild(child,name)) {return found;}}
    return nullptr;
}
struct Fixture {
    QTemporaryDir directory;
    QProcess process;
    QString endpoint;
    QByteArray output;
    uint64_t sequence=0;
    explicit Fixture(bool safe=false,bool pty=false) {
        QStringList args={"--port","0","--state",directory.path()+"/state","--scenario-file",directory.path()+"/control"};
        if(safe) {args<<"--safe-mode";}
        if(pty) {args<<"--pty";}
        process.start(QString::fromUtf8(VDEV_PATH),args);
        if(!process.waitForStarted(10000) || !process.waitForReadyRead(10000)) {throw std::runtime_error("vdev did not start");}
        endpoint=QString::fromUtf8(process.readLine()).trimmed().mid(6);
        if(!endpoint.startsWith("tcp://") && !endpoint.startsWith("serial://")) {throw std::runtime_error("bad vdev endpoint");}
        if(!safe) {
            Session s(endpoint.toStdString());Client c(s);auto info=c.info();
            std::vector<Ref> refs;std::vector<uint32_t> values;
            for(const auto &loop:info.loops) {refs.push_back({5,loop.id,1});values.push_back(10);}
            refs.push_back({0,0,defs::system::kWatchdogMs});values.push_back(1000);c.set(refs,values);
            // A reboot must retain the desktop scheduling allowance too.
            // Otherwise the reconnect case silently returns to a 1 kHz loop
            // with an 8 ms group-swap deadline and a 200 ms watchdog.
            c.system(5);
        }
    }
    ~Fixture() {process.kill();process.waitForFinished(10000);}
    void command(const QString &action) {
        const auto temp=directory.path()+"/next";QFile file(temp);
        if(!file.open(QIODevice::WriteOnly)) {throw std::runtime_error("cannot write control");}
        file.write((QString::number(++sequence)+" "+action+"\n").toUtf8());file.close();
        const auto path=directory.path()+"/control";
        if(QFile::exists(path) && !QFile::remove(path)) {throw std::runtime_error("cannot replace control");}
        if(!QFile::rename(temp,path)) {throw std::runtime_error("cannot publish control");}
        const auto acknowledgement=(QString("SCENARIO %1 %2\n").arg(sequence).arg(action.section(' ',0,0))).toUtf8();
        // The 100 ms poll is not a scheduling guarantee. Wait for this command's
        // acknowledgement, emitted after its effect and stats-file close.
        const auto acknowledged=waitFor([&]{
            output+=process.readAllStandardOutput();output.replace("\r\n","\n");
            return output.contains(acknowledgement) || process.state()==QProcess::NotRunning;
        });
        if(!acknowledged || !output.contains(acknowledgement)) {
            throw std::runtime_error((QString("vdev did not acknowledge %1: %2 %3")
                .arg(sequence).arg(QString::fromUtf8(output),QString::fromUtf8(process.readAllStandardError()))).toStdString());
        }
        output.clear();
    }
    QByteArray stats() {
        command("stats");QFile file(directory.path()+"/control.stats");
        if(!file.open(QIODevice::ReadOnly)) {throw std::runtime_error("acknowledged stats file missing");}
        return file.readAll().replace("\r\n","\n");
    }
    void multiLoop() {
        {Session s(endpoint.toStdString());Client c(s);c.info();const auto extra=c.add(5,"extra").instance;
         const std::array<Ref,2> refs{{{5,extra,1},{5,extra,2}}};const std::array<uint32_t,2> values{{10,1}};
         c.set(refs,values);c.add(112,"first",0);c.add(112,"second",extra);c.system(5);c.system(8);}
        QTest::qWait(150);
    }
};
DeviceSnapshot sample() {
    DeviceSnapshot s;s.info.max_payload=1024;
    s.system_params[defs::system::kLimitsGroupEntries]=uint32_t(2);
    s.configured[{5,3}]={{5,3,255,0,"loop"},{}};
    s.configured[{5,9}]={{5,9,255,0,"other"},{}};
    s.configured[{112,7}]={{112,7,3,0,"motor"},{{1,float(2)}}};
    s.configured[{112,11}]={{112,11,9,0,"second"},{{1,float(4)}}};
    auto &type=s.types[112];type.descriptor={112,1,"discovered.mock"};
    ModelItem param;param.descriptor.id=1;param.descriptor.name="gain";param.descriptor.value_type=ValueType::F32;
    type.items[1]=param;
    for(uint16_t id=10;id<13;++id) {
        ModelItem input;input.descriptor.id=id;input.descriptor.name="input";input.descriptor.kind=ItemKind::Input;
        input.descriptor.value_type=ValueType::F32;type.items[id]=input;
    }
    s.pending=s.configured;s.pending.at({112,7}).instance.label="pending";s.pending.erase({112,11});
    return s;
}
// Answers every core/diag request with an older wire version, as a device
// with incompatible firmware would.
struct OldDevice {
    QTcpServer server;
    std::vector<QTcpSocket *> sockets;
    OldDevice() {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server,&QTcpServer::newConnection,&server,[this] {
            auto *socket=server.nextPendingConnection();sockets.push_back(socket);
            auto buffer=std::make_shared<QByteArray>();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,buffer] {
                *buffer+=socket->readAll();
                while(buffer->size()>=7) {
                    const int n=uint8_t((*buffer)[4])|(uint8_t((*buffer)[5])<<8);
                    if(buffer->size()<7+n+2) {break;}
                    const uint8_t type=uint8_t((*buffer)[3]);const auto payload=buffer->mid(7,n);buffer->remove(0,7+n+2);
                    if((type!=0x01 && type!=0x23) || payload.size()<3) {continue;}
                    const bool core=type==0x01;
                    Bytes reply{uint8_t(core?protocol::core_wire::kVersion-1:2),uint8_t(payload[1]),uint8_t(payload[2])};
                    if(core) {reply.push_back(uint8_t(payload[3]));reply.push_back(0);reply.push_back(0);}
                    else {reply.push_back(0);}
                    Bytes frame(protocol::frameSize(reply.size()));
                    protocol::encodeFrame(frame.data(),frame.size(),uint8_t(type|0x40),reply.data(),reply.size());
                    socket->write(reinterpret_cast<const char *>(frame.data()),qint64(frame.size()));
                }
            });
        });
    }
    QString endpoint() const {return QString("tcp://127.0.0.1:%1").arg(server.serverPort());}
};
std::shared_ptr<DeviceSnapshot> loopSnapshot() {
    auto s=std::make_shared<DeviceSnapshot>();
    auto &loop=s->types[defs::kLoopTypeId];loop.descriptor={defs::kLoopTypeId,1,"loop"};
    ModelItem core;core.descriptor.id=protocol::defs::loop::kCore;core.enums={{0,"core_0"},{1,"core_1"},{2,"any"}};
    loop.items[core.descriptor.id]=core;
    s->configured[{5,0}]={{5,0,0,0,"control"},{{protocol::defs::loop::kRateHz,uint16_t(1000)},{protocol::defs::loop::kCore,uint32_t(1)}}};
    s->configured[{5,4}]={{5,4,4,0,"slow"},{{protocol::defs::loop::kRateHz,uint16_t(10)},{protocol::defs::loop::kCore,uint32_t(2)}}};
    s->info.loops={{0,"control",1000,1},{4,"slow",10,0}};
    StatusFrame status;status.overruns={{4,7}};s->status=status;
    s->codes={{0x04,0x000B,1,2,4,"LoopStalled"},{0x02,0x0001,2,1,0,"CrcError"}};
    s->diagnostics={{0x02,0xFF,0x0001,1,40},{0x04,0xFF,0x000B,3,1}};
    return s;
}
GroupList available() {
    GroupList list;list.entry_capacity=64;list.command_capacity=5;
    for(uint8_t id=0;id<5;++id) {Group g;g.kind=GroupKind::Command;g.id=id;list.groups.push_back(g);}
    return list;
}
}
class BackendTests final : public QObject {
    Q_OBJECT
private slots:
    void scenarioFailureUnwinds() {
        Fixture fixture;
        QVERIFY(fixture.stats().contains("disarms 0\n"));
        fixture.process.kill();QVERIFY(fixture.process.waitForFinished(10000));
        QVERIFY_EXCEPTION_THROWN(fixture.stats(),std::runtime_error);
    }
    void planning() {
        auto s=sample();auto groups=available();
        auto plan=planCommands(s,groups);QCOMPARE(plan.size(),size_t(4));
        QCOMPARE(plan[0].loop,3);QCOMPARE(plan[2].loop,9);QCOMPARE(plan[0].refs.size(),size_t(2));
        groups.groups[0].configured=true;groups.groups[0].refs={{112,7,10}};
        QVERIFY_EXCEPTION_THROWN(planCommands(s,groups),Error);
        groups=available();groups.groups.resize(3);QVERIFY_EXCEPTION_THROWN(planCommands(s,groups),Error);
        s.info.max_payload=11;groups=available();Group extra;extra.kind=GroupKind::Command;extra.id=5;groups.groups.push_back(extra);
        plan=planCommands(s,groups);QCOMPARE(plan.size(),size_t(6));
    }
    void emptyAndUnavailable() {
        auto s=sample();s.configured.erase({112,7});s.configured.erase({112,11});
        auto result=planCommands(s,available());QCOMPARE(result.size(),size_t(1));QVERIFY(result[0].refs.empty());
        s.info.flags=8;QVERIFY_EXCEPTION_THROWN(planCommands(s,available()),Error);
        s.info.flags=0;s.configured.clear();QVERIFY_EXCEPTION_THROWN(planCommands(s,available()),Error);
    }
    void modelsAndErrors() {
        auto s=std::make_shared<DeviceSnapshot>(sample());InstanceTree tree;
        QAbstractItemModelTester tester(&tree,QAbstractItemModelTester::FailureReportingMode::QtTest);
        tree.reset(s);QCOMPARE(tree.rowCount(),2);QCOMPARE(tree.rowCount(tree.index(0,0)),1);
        auto child=tree.index(0,0,tree.index(0,0));QCOMPARE(tree.data(child,InstanceTree::Key).toString(),"112:7");
        QCOMPARE(tree.parent(child),tree.index(0,0));
        tree.reset(s,true);QCOMPARE(tree.data(tree.index(0,0,tree.index(0,0)),InstanceTree::Label).toString(),"pending");
        QCOMPARE(tree.rowCount(tree.index(1,0)),0);
        DescriptorRows rows;QAbstractItemModelTester rowTester(&rows,QAbstractItemModelTester::FailureReportingMode::QtTest);
        rows.reset(s,112,7);QCOMPARE(rows.rowCount(),4);
        QCOMPARE(rows.data(rows.index(0),DescriptorRows::ValueBits).toUInt(),valueBits(Value(float(2))));
        QVERIFY(!rows.data(rows.index(1),DescriptorRows::ValueBits).isValid());
        rows.reset(s,112,11,true);QCOMPARE(rows.rowCount(),0);
        Error error(ErrorKind::Timeout,"uncertain",3,4);error.family=1;error.op=2;error.has_request_context=true;error.outcome_unknown=true;
        const auto fields=errorFields(error);QVERIFY(fields["outcomeUnknown"].toBool());QCOMPARE(fields["op"].toInt(),2);
    }
    void deviceModelsAndText() {
        QCOMPARE(LoopTable::budget(250,1000),0.25);QCOMPARE(LoopTable::budget(5000,10),0.05);QCOMPARE(LoopTable::budget(10,0),0.0);
        const auto s=loopSnapshot();
        LoopTable loops;QAbstractItemModelTester loopTester(&loops,QAbstractItemModelTester::FailureReportingMode::QtTest);
        loops.reset(s);QCOMPARE(loops.rowCount(),2);
        QCOMPARE(loops.data(loops.index(1),LoopTable::LoopId).toInt(),4); // keyed by id, gaps allowed
        QCOMPARE(loops.data(loops.index(0),LoopTable::Core).toString(),"core_1");
        QCOMPARE(loops.data(loops.index(1),LoopTable::Core).toString(),"any");
        QVERIFY(loops.data(loops.index(0),LoopTable::Realtime).toBool());
        QVERIFY(!loops.data(loops.index(0),LoopTable::Overruns).isValid());
        QCOMPARE(loops.data(loops.index(1),LoopTable::Overruns).toUInt(),7u);
        QVERIFY(!loops.data(loops.index(0),LoopTable::Budget).isValid());
        loops.setRunTime(0,LoopRunTime{400,900,1});
        QCOMPARE(loops.data(loops.index(0),LoopTable::Budget).toDouble(),0.4);
        QCOMPARE(loops.data(loops.index(0),LoopTable::MaxRunUs).toUInt(),900u);
        loops.setRunTime(4,std::nullopt,"No free subscription slot");
        QCOMPARE(loops.data(loops.index(1),LoopTable::RunTimeNote).toString(),"No free subscription slot");
        loops.reset(s);QCOMPARE(loops.data(loops.index(0),LoopTable::LastRunUs).toUInt(),400u); // kept across metadata
        FaultList faults;QAbstractItemModelTester faultTester(&faults,QAbstractItemModelTester::FailureReportingMode::QtTest);
        faults.reset(s);QCOMPARE(faults.rowCount(),2);QCOMPARE(faults.activeCount(),1);
        QCOMPARE(faults.data(faults.index(0),FaultList::Name).toString(),"LoopStalled"); // active first
        QVERIFY(faults.data(faults.index(0),FaultList::Disarms).toBool());
        QCOMPARE(faults.data(faults.index(0),FaultList::SourceName).toString(),"core");
        QCOMPARE(faults.data(faults.index(1),FaultList::Count).toUInt(),40u);
        QVERIFY(!faults.data(faults.index(1),FaultList::InstanceId).isValid());
        QVERIFY(refusalText(uint8_t(defs::Result::Armed),uint8_t(defs::Detail::LockedWhileArmed)).contains("armed"));
        QVERIFY(refusalText(uint8_t(defs::Result::InvalidArgument),uint8_t(defs::Detail::NoSavedConfig)).contains("no saved"));
        QVERIFY(refusalText(200,200).contains("200"));
        QCOMPARE(resetReasonText(4),QString("Panic"));QVERIFY(resetReasonText(99).contains("99"));
        QCOMPARE(disarmReasonText(2),QString("Command watchdog"));
        Error uncertain(ErrorKind::Timeout,"t");uncertain.outcome_unknown=true;QVERIFY(errorText(uncertain).contains("may have"));
        LocalConnection c;
        QVERIFY(c.nameProblem("bench-left").isEmpty());QVERIFY(c.nameProblem("").isEmpty());
        QVERIFY(!c.nameProblem("seventeen-bytes-x").isEmpty());QVERIFY(!c.nameProblem(QString("a\tb")).isEmpty());
        QVERIFY(c.nameProblem(QString::fromUtf8("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9")).isEmpty()); // 16 bytes
        QVERIFY(!c.nameProblem(QString::fromUtf8("\xc2\x85")).isEmpty()); // C1 control
    }
    // Pure Layout rules on a snapshot with gaps in loop ids, every loop mask,
    // wiring, a source, GPIO pins and each kind of staged change.
    void layoutLogic() {
        using defs::ItemFlag;
        auto s=std::make_shared<DeviceSnapshot>();
        auto item=[](uint16_t id,const char *name,ItemKind kind,ValueType type,uint8_t flags=0) {
            ModelItem i;i.descriptor.id=id;i.descriptor.name=name;i.descriptor.kind=kind;i.descriptor.value_type=type;
            i.descriptor.flags=flags;return i;
        };
        const uint8_t reboot=uint8_t(ItemFlag::RebootRequired),gpio=uint8_t(ItemFlag::Gpio)|reboot;
        auto &system=s->types[0];system.descriptor={0,0,"system"};
        system.items[defs::system::kLimitsLoops]=item(defs::system::kLimitsLoops,"limits.loops",ItemKind::Param,ValueType::U8,4);
        system.items[defs::system::kLimitsInstances]=item(defs::system::kLimitsInstances,"limits.instances",ItemKind::Param,ValueType::U8,4);
        s->system_params[defs::system::kLimitsLoops]=uint8_t(4);s->system_params[defs::system::kLimitsInstances]=uint8_t(5);
        auto &loop=s->types[5];loop.descriptor={5,0,"loop"};
        loop.items[1]=item(1,"rate_hz",ItemKind::Param,ValueType::U16);
        loop.items[2]=item(2,"realtime",ItemKind::Param,ValueType::Bool,reboot);
        auto &ctrl=s->types[32];ctrl.descriptor={32,1,"controller.pid"};
        ctrl.items[1]=item(1,"actuator",ItemKind::Param,ValueType::U32,reboot);ctrl.items[1].descriptor.ref="actuator";
        ctrl.items[2]=item(2,"vel.kp",ItemKind::Param,ValueType::F32);
        ctrl.items[2].descriptor.has_min=ctrl.items[2].descriptor.has_max=true;
        ctrl.items[2].descriptor.max_bits=valueBits(Value(float(10)));
        ctrl.items[3]=item(3,"mode",ItemKind::Param,ValueType::Enum);ctrl.items[3].enums={{0,"open"},{1,"velocity"}};
        auto &act=s->types[112];act.descriptor={112,1,"actuator.m"};
        act.items[1]=item(1,"pin",ItemKind::Param,ValueType::U8,gpio);act.items[1].descriptor.default_bits=255;
        act.items[0x100]=item(0x100,"duty",ItemKind::Signal,ValueType::F32);
        auto &imu=s->types[114];imu.descriptor={114,2,"imu.i"};
        imu.items[0x100]=item(0x100,"yaw",ItemKind::Signal,ValueType::F32);
        imu.items[0x101]=item(0x101,"count",ItemKind::Signal,ValueType::I32);
        auto &fusion=s->types[115];fusion.descriptor={115,3,"fusion.f"};
        fusion.items[1]=item(1,"feedback",ItemKind::Param,ValueType::U32,reboot|uint8_t(ItemFlag::Source));
        fusion.items[0x100]=item(0x100,"feedback.value",ItemKind::Signal,ValueType::F32);
        fusion.items[0x101]=item(0x101,"feedback.age_us",ItemKind::Signal,ValueType::U32);
        auto add=[&](ModelLayout &l,uint8_t t,uint8_t i,uint8_t loopId,const char *label,std::map<uint16_t,Value> params) {
            l[{t,i}]={{t,i,uint8_t(t==5?255:loopId),0,label},std::move(params)};
        };
        add(s->configured,5,0,0,"control",{{1,uint16_t(1000)},{2,true}});
        add(s->configured,5,3,0,"slow",{{1,uint16_t(10)},{2,false}});
        add(s->configured,32,0,0,"drive",{{1,uint32_t(defs::wiringRef(112,0))},{2,float(0.1f)},{3,EnumValue{1}}});
        add(s->configured,112,0,0,"pwm",{{1,uint8_t(4)}});
        add(s->configured,112,1,0,"old",{{1,uint8_t(255)}});
        add(s->configured,114,0,3,"imu",{});
        add(s->configured,115,0,3,"odom",{{1,uint32_t(defs::sourceRef(114,0,0x100))}});
        s->pending=s->configured;
        s->pending.erase({112,1});                                    // pending remove
        s->pending.at({114,0}).instance.label="imu_main";              // label change
        add(s->pending,112,2,0,"aux",{{1,uint8_t(5)}});                // pending add, pin 5
        add(s->pending,112,3,3,"far",{{1,uint8_t(255)}});              // another loop
        s->pending.at({5,3}).params[2]=true;                           // reboot param change

        // Values and editors.
        QCOMPARE(formatValue(ctrl.items[2],valueBits(Value(float(0.1f)))),QString("0.1"));
        QCOMPARE(formatValue(act.items[1],255),QString("No pin"));
        QCOMPARE(formatValue(ctrl.items[3],1),QString("velocity"));
        QCOMPARE(*parseValueText(act.items[1],"").bits,255u);
        QCOMPARE(*parseValueText(act.items[1],"12").bits,12u);
        QVERIFY(!parseValueText(act.items[1],"300").bits);
        QVERIFY(!parseValueText(ctrl.items[2],"abc").bits);QVERIFY(!parseValueText(ctrl.items[2],"abc").problem.isEmpty());
        QCOMPARE(*parseValueText(ctrl.items[3],"velocity").bits,1u);
        QVERIFY(rangeText(ctrl.items[2]).contains("10"));
        // Loops by real-time compatibility; the default is the lowest compatible id.
        QCOMPARE(int(*defaultLoop(*s,32)),0);
        QVERIFY(!defaultLoop(*s,114)); // slow is real-time in the pending layout
        QVERIFY(!loopChoices(*s,114)[0].compatible);QVERIFY(!loopChoices(*s,114)[1].compatible);
        QCOMPARE(loopChoices(*s,115).size(),size_t(2));QVERIFY(loopChoices(*s,115)[1].compatible);
        QCOMPARE(typeNeed(1),QString("needs a real-time loop"));QCOMPARE(typeNeed(2),QString("not in a real-time loop"));
        QCOMPARE(typeNeed(3),QString("any loop"));
        // Remove follows the firmware: wired, read as a source, loop in use.
        QVERIFY(removeBlocker(*s,{112,0}).contains("drive is wired to it"));
        QVERIFY(removeBlocker(*s,{114,0}).contains("odom reads it as a source"));
        QVERIFY(removeBlocker(*s,{5,3}).contains("imu_main"));
        QVERIFY(removeBlocker(*s,{112,2}).isEmpty());QVERIFY(removeBlocker(*s,{32,0}).isEmpty());
        // Wiring: the category, same loop enabled, another loop disabled with the reason.
        const auto wires=wiringCandidates(*s,{32,0},ctrl.items[1].descriptor);
        QCOMPARE(wires.size(),size_t(4));QCOMPARE(wires[0].value,0u);
        QVERIFY(std::none_of(wires.begin(),wires.end(),[](const Candidate &c){return c.text=="old";})); // removed
        const auto far=std::find_if(wires.begin(),wires.end(),[](const Candidate &c){return c.text=="far";});
        QVERIFY(!far->enabled);QCOMPARE(far->reason,QString("slow loop; drive runs in control"));
        // Sources: matching value type in any loop, never the consumer itself.
        const auto targets=sourceCandidates(*s,{115,0},fusion.items[1].descriptor);
        QStringList names;for(const auto &c:targets) {names<<c.text;}
        QVERIFY(names.contains("imu_main.yaw"));QVERIFY(names.contains("pwm.duty"));
        QVERIFY(!names.contains("imu_main.count"));QVERIFY(!names.join(" ").contains("odom."));
        QCOMPARE(sourceText(*s,defs::sourceRef(114,0,0x100)),QString("imu_main.yaw"));
        QCOMPARE(wiringText(*s,0),QString("Not wired"));QCOMPARE(sourceText(*s,0),QString("Not connected"));
        // Staged changes, configured against pending.
        QStringList kinds;for(const auto &c:pendingChanges(*s)) {kinds<<c.kind+":"+c.subject;}
        QVERIFY(kinds.contains("remove:old"));QVERIFY(kinds.contains("add:aux"));QVERIFY(kinds.contains("label:imu_main"));
        QVERIFY(kinds.contains("param:slow.realtime"));QVERIFY(kinds.contains("param:aux.pin")); // differs from the default
        QVERIFY(!kinds.contains("param:far.pin"));
        const auto counts=layoutCounts(*s);
        QCOMPARE(counts.loops,2);QCOMPARE(counts.loopLimit,4);QCOMPARE(counts.instances,6);QCOMPARE(counts.instanceLimit,5);
        // A GpioConflict names the holder from the pending layout.
        QCOMPARE(gpioOwner(*s,{112,2,1},4),QString("pwm (pin)"));QVERIFY(gpioOwner(*s,{112,0,1},4).isEmpty());
        Error conflict(ErrorKind::Rejected,"gpio",uint8_t(defs::Result::InvalidArgument),uint8_t(defs::Detail::GpioConflict));
        QCOMPARE(layoutRefusal(*s,conflict,Ref{112,2,1},4u),QString("GPIO 4 is already used by pwm (pin)"));
        Error referenced(ErrorKind::Rejected,"ref",uint8_t(defs::Result::InvalidArgument),uint8_t(defs::Detail::Referenced));
        QVERIFY(layoutRefusal(*s,referenced,{},{},InstanceKey{112,0}).contains("drive"));
        QVERIFY(!refusalText(1,uint8_t(defs::Detail::LoopMismatch)).isEmpty());
        // The tree: loops by id, modules indented under what is wired to them.
        LayoutTree tree;QAbstractItemModelTester treeTester(&tree,QAbstractItemModelTester::FailureReportingMode::QtTest);
        tree.reset(s);
        QCOMPARE(tree.keys(),(QStringList{"5:0","32:0","112:0","112:1","112:2","5:3","112:3","114:0","115:0"}));
        QCOMPARE(tree.data(tree.index(2),LayoutTree::Depth).toInt(),2);
        QCOMPARE(tree.data(tree.index(3),LayoutTree::Pending).toString(),QString("remove"));
        QCOMPARE(tree.data(tree.index(4),LayoutTree::Pending).toString(),QString("add"));
        QCOMPARE(tree.data(tree.index(4),LayoutTree::Health).toInt(),255);
        QCOMPARE(tree.data(tree.index(5),LayoutTree::Pending).toString(),QString("change"));
        QVERIFY(!tree.data(tree.index(5),LayoutTree::Realtime).toBool()); // the running value until the reboot
        tree.setBudgets({{0,0.25}});QCOMPARE(tree.data(tree.index(0),LayoutTree::Budget).toDouble(),0.25);
        QVERIFY(!tree.data(tree.index(5),LayoutTree::HasBudget).toBool());
        // The param table: wiring/source params are listed by their cards.
        ParamTable params;QAbstractItemModelTester paramTester(&params,QAbstractItemModelTester::FailureReportingMode::QtTest);
        QHash<QString,ParamEdit> edits{{"32:0:2",{"rejected","no","abc"}}};
        params.reset(s,InstanceKey{32,0},false,edits,{"32:0:2"},0,{});
        QCOMPARE(params.rowCount(),2);QCOMPARE(params.total(),2);QCOMPARE(params.changedCount(),1);
        QCOMPARE(params.data(params.index(0),ParamTable::Editor).toString(),QString("enum"));
        const auto kp=params.index(1);
        QCOMPARE(params.data(kp,ParamTable::Group).toString(),QString("vel"));
        QCOMPARE(params.data(kp,ParamTable::EditState).toString(),QString("rejected"));
        QVERIFY(params.data(kp,ParamTable::Unsaved).toBool());
        params.reset(s,InstanceKey{32,0},true,{},{},2,{});QCOMPARE(params.rowCount(),0); // nothing pending
        params.reset(s,InstanceKey{5,3},true,{},{},2,{});QCOMPARE(params.rowCount(),1);
        QCOMPARE(params.data(params.index(0),ParamTable::PendingText).toString(),QString("On"));
        QCOMPARE(params.data(params.index(0),ParamTable::ValueText).toString(),QString("Off"));
        params.reset(s,InstanceKey{112,2},false,{},{},0,"pi");QCOMPARE(params.rowCount(),1);
        QVERIFY(params.data(params.index(0),ParamTable::IsPending).toBool()); // a pending add is all staged
        QCOMPARE(params.data(params.index(0),ParamTable::Editor).toString(),QString("gpio"));
        params.reset(s,InstanceKey{112,1},false,{},{},0,{});QVERIFY(params.data(params.index(0),ParamTable::ReadOnly).toBool());
        params.reset(s,InstanceKey{32,0},true,{},{},0,"mode");QCOMPARE(params.rowCount(),1);
    }
    // Layout ops against vdev: each sent once, refusals kept on their row and
    // not staged, the armed lock, Discard all and Save and reboot.
    void layoutOperations() {
        Fixture fixture;LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        QSignalSpy results(&c,&Connection::layoutResult);
        auto last=[&](const QString &op) {
            return waitFor([&]{return !results.isEmpty() && results.last().at(0).toString()==op && !c.configBusy();});
        };
        auto row=[&](const QString &key)->QModelIndex {
            for(int i=0;i<c.params()->rowCount();++i) {if(c.params()->index(i,0).data(ParamTable::Key).toString()==key) {return c.params()->index(i,0);}}
            return {};
        };
        auto pendingCount=[&]{return c.layout()["pendingChanges"].toList().size();};
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        QCOMPARE(c.layout()["counts"].toMap()["loops"].toInt(),2);
        QCOMPARE(c.selectedNode(),QString("5:0"));QCOMPARE(pendingCount(),0);
        // Add loop: label, a non-default priority, the core default (any).
        c.addLoop("fusion",{{"4","14"}});QVERIFY(last("addLoop"));
        QVERIFY2(results.last().at(1).toBool(),qPrintable(results.last().at(2).toString()));
        QVERIFY(waitFor([&]{return c.selectedNode()=="5:2" && c.layoutTree()->rowCount()==3;}));
        auto pending=c.snapshot()->pending.at({5,2}).params;
        QCOMPARE(valueBits(pending.at(4)),14u);QCOMPARE(valueBits(pending.at(defs::loop::kCore)),2u);
        // Instances: the loop choice, and a type the loop does not suit (not staged).
        c.addInstance(112,"left_pwm",0);QVERIFY(last("addInstance"));QVERIFY(results.last().at(1).toBool());
        c.addInstance(113,"left_enc",0);QVERIFY(last("addInstance"));
        c.addInstance(32,"left_drive",0);QVERIFY(last("addInstance"));
        c.addInstance(115,"odom",2);QVERIFY(last("addInstance"));
        QVERIFY(waitFor([&]{return c.snapshot()->pending.size()==7;}));
        const auto before=pendingCount();
        c.addInstance(114,"imu",0);QVERIFY(last("addInstance"));QVERIFY(!results.last().at(1).toBool());
        QVERIFY(results.last().at(2).toString().contains("loop"));
        c.addInstance(112,"left_pwm",0);QVERIFY(last("addInstance"));QVERIFY(!results.last().at(1).toBool()); // LabelInUse
        QVERIFY(results.last().at(2).toString().contains("label"));
        QTest::qWait(200);QCOMPARE(pendingCount(),before);
        // Wiring and sources: accepted targets pending; BadRef kept on the row.
        c.setParamValue("32:0:1",112u<<8);
        QVERIFY(waitFor([&]{return valueBits(c.snapshot()->pending.at({32,0}).params.at(1))==(112u<<8);}));
        c.selectNode("115:0");
        c.setParamValue("115:0:1",defs::sourceRef(113,0,0x0101));
        QVERIFY(waitFor([&]{return valueBits(c.snapshot()->pending.at({115,0}).params.at(1))==defs::sourceRef(113,0,0x0101);}));
        const auto sources=c.layout()["sources"].toList();QCOMPARE(sources.size(),2);
        QCOMPARE(sources[0].toMap()["valueText"].toString(),QString("left_enc.position"));
        QCOMPARE(sources[0].toMap()["chip"].toString(),QString("f32"));
        c.setParamValue("115:0:2",defs::sourceRef(113,0,0x0001)); // a param: BadRef
        QVERIFY(waitFor([&]{return c.layout()["sources"].toList()[1].toMap()["editState"].toString()=="rejected";}));
        QVERIFY(c.layout()["sources"].toList()[1].toMap()["editNote"].toString().contains("target"));
        QCOMPARE(valueBits(c.snapshot()->pending.at({115,0}).params.at(2)),0u);
        // Remove follows the firmware: refused locally with the reason, nothing sent.
        c.removeNode("112:0");QVERIFY(last("remove"));QVERIFY(!results.last().at(1).toBool());
        QVERIFY(results.last().at(2).toString().contains("left_drive"));
        c.selectNode("5:2");QVERIFY(c.layout()["node"].toMap()["removeBlocked"].toString().contains("odom"));
        // Labels.
        c.setNodeLabel("113:0","left_pwm");QVERIFY(last("label"));QVERIFY(!results.last().at(1).toBool());
        c.setNodeLabel("113:0","left_enc2");QVERIFY(last("label"));QVERIFY(results.last().at(1).toBool());
        // Live and reboot params on a running loop: clamped, applied, pending.
        c.selectNode("5:1");QVERIFY(waitFor([&]{return row("5:1:1").isValid();}));
        c.setParamText("5:1:1","0");
        QVERIFY(waitFor([&]{return row("5:1:1").data(ParamTable::EditState).toString()=="clamped";}));
        QVERIFY(row("5:1:1").data(ParamTable::EditNote).toString().contains("1"));
        c.setParamText("5:1:1","10");
        QVERIFY(waitFor([&]{return row("5:1:1").data(ParamTable::EditState).toString()=="applied" && row("5:1:1").data(ParamTable::ValueText).toString()=="10";}));
        QVERIFY(row("5:1:1").data(ParamTable::Unsaved).toBool());QVERIFY(c.unsavedChanges().size()>=2);
        c.setParamText("5:1:1","fast");QCOMPARE(row("5:1:1").data(ParamTable::EditState).toString(),QString("rejected")); // not sent
        c.clearParamEdit("5:1:1");QVERIFY(row("5:1:1").data(ParamTable::EditState).toString().isEmpty());
        c.setParamText("5:1:4","12");
        QVERIFY(waitFor([&]{return row("5:1:4").data(ParamTable::IsPending).toBool();}));
        QCOMPARE(row("5:1:4").data(ParamTable::PendingText).toString(),QString("12"));
        // Discard all drops every staged change; live changes stay.
        QVERIFY(pendingCount()>=8);QVERIFY(!c.armEnabled()); // a pending reboot blocks ARM
        c.discardPending();QVERIFY(last("discard"));QVERIFY(results.last().at(1).toBool());
        QVERIFY(waitFor([&]{return pendingCount()==0 && c.layoutTree()->rowCount()==2;}));
        QCOMPARE(c.selectedNode(),QString("5:1"));QCOMPARE(row("5:1:1").data(ParamTable::ValueText).toString(),QString("10"));
        // The reboot notice no longer lists the discarded staged changes.
        QVERIFY(!c.unsavedChanges().isEmpty());
        for(const auto &line:c.unsavedChanges()) {QVERIFY2(line.contains("sensor.rate_hz"),qPrintable(line));}
        // Armed: the layout and reboot params are locked; locked live params refused.
        QVERIFY(waitFor([&]{return c.armEnabled();}));
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        QVERIFY(waitFor([&]{return row("5:1:1").data(ParamTable::Locked).toBool();}));
        QVERIFY(c.layoutBlocked().contains("armed"));QVERIFY(c.layout()["addLoopBlocked"].toString().contains("armed"));
        c.addInstance(112,"later",0);QVERIFY(last("addInstance"));QVERIFY(results.last().at(2).toString().contains("armed"));
        c.setParamText("5:1:4","13");QVERIFY(row("5:1:4").data(ParamTable::EditNote).toString().contains("armed"));
        c.setParamText("5:1:1","20");QVERIFY(row("5:1:1").data(ParamTable::EditNote).toString().contains("armed"));
        QTest::qWait(200);QCOMPARE(pendingCount(),0);QCOMPARE(c.armState(),1);
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0 && c.layoutBlocked().isEmpty();}));
        // Save and reboot applies a staged add; the reboot is never repeated.
        c.addInstance(112,"left_pwm",0);QVERIFY(last("addInstance"));QVERIFY(results.last().at(1).toBool());
        c.saveAndReboot();
        QVERIFY(waitFor([&]{return c.ready() && !c.reconnecting() && c.rebootNotice()["visible"].toBool();},15000));
        QVERIFY(c.snapshot()->configured.contains({112,0}));QCOMPARE(c.snapshot()->configured.at({112,0}).instance.state,uint8_t(0));
        QCOMPARE(pendingCount(),0);QVERIFY(c.layoutTree()->rowCount()==3);
        QVERIFY(fixture.stats().contains("reboots 1\n"));
        c.dismissRebootNotice();
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
        QCOMPARE(c.layoutTree()->rowCount(),0);
    }
    void recentDevices() {
        QTemporaryDir dir;RecentDevices recent(dir.filePath("gui.ini"));
        QVERIFY(recent.list().isEmpty());
        for(int i=0;i<10;++i) {recent.remember(QString("%1").arg(i,12,10,QChar('0')),QString("dev%1").arg(i),QString("serial://COM%1").arg(i),921600,"fw");}
        auto list=recent.list();QCOMPARE(list.size(),RecentDevices::kLimit);
        QCOMPARE(list[0].toMap()["name"].toString(),"dev9");
        recent.remember(QString("%1").arg(5,12,10,QChar('0')),"renamed","serial://COM5",115200,"fw2");
        list=recent.list();QCOMPARE(list[0].toMap()["name"].toString(),"renamed");QCOMPARE(list[0].toMap()["baud"].toInt(),115200);
        QCOMPARE(list.size(),RecentDevices::kLimit);
        QCOMPARE(recent.onEndpoint("serial://COM5")["name"].toString(),"renamed");
        recent.forget(QString("%1").arg(5,12,10,QChar('0')));QVERIFY(recent.onEndpoint("serial://COM5").isEmpty());
        std::vector<SerialPortInfo> ports(2);ports[0].name="COM5";ports[0].endpoint="serial://COM5";
        ports[1].name="COM9";ports[1].endpoint="serial://COM9";ports[1].vid=0x1a86;ports[1].pid=0x55d3;ports[1].description="USB-Enhanced-SERIAL CH343";
        const auto rows=portRows(ports,recent,"serial://COM9");
        QVERIFY(!rows[0].toMap().contains("deviceId"));QVERIFY(!rows[0].toMap()["inUse"].toBool());
        QCOMPARE(rows[1].toMap()["deviceName"].toString(),"dev9");QCOMPARE(rows[1].toMap()["usbId"].toString(),"1a86:55d3");
        QVERIFY(rows[1].toMap()["inUse"].toBool());
    }
    void protocolMismatch() {
        OldDevice device;LocalConnection c;
        QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(device.endpoint());
        QVERIFY(waitFor([&]{return !c.mismatch().isEmpty() && !c.loading();},15000));
        QCOMPARE(c.mismatch()["device"].toInt(),int(protocol::core_wire::kVersion)-1);
        QCOMPARE(c.mismatch()["host"].toInt(),int(protocol::core_wire::kVersion));
        QVERIFY(!c.linked());QVERIFY(c.recentDevices().isEmpty());
    }
    void deviceActionsAndRebootNotice() {
        Fixture fixture;LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        QCOMPARE(c.info()["hostProtocol"].toInt(),int(protocol::core_wire::kVersion));
        QVERIFY(!c.info()["firmware"].toString().isEmpty());QVERIFY(!c.limits().isEmpty());
        QCOMPARE(c.recentDevices().size(),1);QCOMPARE(c.recentDevices()[0].toMap()["id"].toString(),c.deviceId());
        // Loop statistics use GUI-owned subscriptions on free slots.
        QVERIFY(c.loops()->rowCount()>=1);QVERIFY(waitFor([&]{return !c.ownedSubscriptions().empty();}));
        QVERIFY(waitFor([&]{return c.loops()->index(0,0).data(LoopTable::HasRunTime).toBool();},5000));
        QCOMPARE(c.actionBlocked()["save"].toString(),QString("There are no unsaved changes"));
        QVERIFY(c.actionBlocked()["reboot"].toString().isEmpty());
        c.rename("bench-a");QVERIFY(waitFor([&]{return c.deviceName()=="bench-a" && c.unsaved() && !c.configBusy();}));
        QCOMPARE(c.unsavedChanges().size(),1);QVERIFY(c.actionBlocked()["save"].toString().isEmpty());
        QVERIFY(waitFor([&]{return c.recentDevices()[0].toMap()["name"].toString()=="bench-a";}));
        c.revert();QVERIFY(waitFor([&]{return c.deviceName().isEmpty() && !c.unsaved() && !c.configBusy();}));
        QVERIFY(c.unsavedChanges().isEmpty());
        // While armed, persistence and reboot are refused with a reason.
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        c.rename("bench-a");QVERIFY(waitFor([&]{return c.deviceName()=="bench-a" && !c.configBusy();})); // live while armed
        QVERIFY(c.actionBlocked()["save"].toString().contains("armed"));QVERIFY(c.actionBlocked()["reboot"].toString().contains("armed"));
        c.reboot();QCOMPARE(c.armState(),1);QVERIFY(c.lastError()["message"].toString().contains("armed"));
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0 && c.armEnabled();}));
        c.save();QVERIFY(waitFor([&]{return !c.unsaved() && !c.configBusy();}));QVERIFY(c.unsavedChanges().isEmpty());
        c.rename("bench-b");QVERIFY(waitFor([&]{return c.deviceName()=="bench-b" && c.unsaved() && !c.configBusy();}));
        // REBOOT closes vdev's TCP link; the GUI reconnects (never re-sends
        // REBOOT) and reports what the reboot discarded.
        c.reboot();
        QVERIFY(!c.armEnabled());QCOMPARE(c.armBlockedReason(),QString("The device is rebooting"));
        QVERIFY(waitFor([&]{return c.reconnecting();},5000));
        QVERIFY(waitFor([&]{return c.ready() && !c.reconnecting();},15000));
        QVERIFY(c.rebootNotice()["visible"].toBool());QVERIFY(c.rebootNotice()["expected"].toBool());
        QCOMPARE(c.rebootNotice()["cause"].toString(),"Reboot");
        const auto lost=c.rebootNotice()["lost"].toStringList();QCOMPARE(lost.size(),1);QVERIFY(lost[0].contains("bench-b"));
        QCOMPARE(c.deviceName(),"bench-a");QVERIFY(c.unsavedChanges().isEmpty());
        QTest::qWait(1500);
        QVERIFY2(c.lastError().isEmpty(),qPrintable(QString("%1 family %2 op %3 result %4 detail %5").arg(c.lastError()["message"].toString())
            .arg(c.lastError()["family"].toInt()).arg(c.lastError()["op"].toInt()).arg(c.lastError()["result"].toInt()).arg(c.lastError()["detail"].toInt())));
        c.dismissRebootNotice();QVERIFY(c.rebootNotice().isEmpty());
        QVERIFY(waitFor([&]{return c.armEnabled();}));
        // Save and reboot loses nothing, although STATUS still showed dirty.
        c.rename("bench-c");QVERIFY(waitFor([&]{return c.deviceName()=="bench-c" && c.unsaved() && !c.configBusy();}));
        c.saveAndReboot();QVERIFY(c.actionBlocked()["reboot"].toString().isEmpty() || c.configBusy());
        QVERIFY(waitFor([&]{return c.ready() && !c.reconnecting() && c.rebootNotice()["visible"].toBool();},15000));
        QCOMPARE(c.rebootNotice()["cause"].toString(),"Save and reboot");
        QVERIFY(c.rebootNotice()["lost"].toStringList().isEmpty());QCOMPARE(c.deviceName(),"bench-c");
        QVERIFY(c.actionBlocked()["reboot"].toString().isEmpty()); // the expectation ended with the notice
        c.dismissRebootNotice();QVERIFY(waitFor([&]{return c.armEnabled();}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
        Session s(fixture.endpoint.toStdString());Client client(s);client.info();
        for(const auto &g:client.groups().groups) {QVERIFY2(!g.configured,qPrintable(QString("kind %1 group %2 lastError %3").arg(int(g.kind)).arg(g.id).arg(c.lastError()["message"].toString())));} // subscriptions released too
    }
    void foreignSubscriptionsAreNotReused() {
        Fixture fixture;
        fixture.multiLoop(); // two loops, so one of them finds no free slot
        // Another client left every subscription slot but one configured
        // (vdev serves one TCP client at a time; the groups outlive it).
        std::vector<uint8_t> taken;
        {
            Session again(fixture.endpoint.toStdString());Client second(again);const auto info2=second.info();
            QVERIFY(info2.loops.size()>=2);
            const auto groups=second.groups();
            for(const auto &g:groups.groups) {
                if(g.kind!=GroupKind::Subscription) {continue;}
                if(taken.size()+1>=groups.subscription_capacity) {break;}
                const std::array<Ref,1> refs{{{5,info2.loops[0].id,protocol::defs::loop::kTicks}}};
                second.subConfigure(g.id,info2.loops[0].id,1,refs);taken.push_back(g.id);
            }
        }
        LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));c.connectTo(fixture.endpoint);
        const auto notes=[&] {
            QString text=c.lastError()["message"].toString();
            for(int i=0;i<c.loops()->rowCount();++i) {text+=" | "+c.loops()->index(i,0).data(LoopTable::RunTimeNote).toString();}
            return text;
        };
        QVERIFY2(waitFor([&]{return c.ready() && !c.ownedSubscriptions().empty();}),qPrintable(notes()));
        QCOMPARE(c.ownedSubscriptions().size(),size_t(1));
        QVERIFY(std::find(taken.begin(),taken.end(),c.ownedSubscriptions()[0])==taken.end());
        QVERIFY(waitFor([&]{
            for(int i=0;i<c.loops()->rowCount();++i) {
                if(c.loops()->index(i,0).data(LoopTable::RunTimeNote).toString()=="No free subscription slot") {return true;}
            }
            return false;
        }));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        Session after(fixture.endpoint.toStdString());Client check(after);check.info();
        int stillTaken=0;
        for(const auto &g:check.groups().groups) {
            if(g.kind==GroupKind::Subscription && g.configured) {
                ++stillTaken;
                QVERIFY2(std::find(taken.begin(),taken.end(),g.id)!=taken.end(),
                    qPrintable(QString("group %1 loop %2 epoch %3 refs %4: %5").arg(g.id).arg(g.loop).arg(g.epoch).arg(g.refs.size()).arg(c.lastError()["message"].toString())));
            }
        }
        QCOMPARE(stillTaken,int(taken.size()));
    }
    void factoryResetAndSafeModeExit() {
        {
            Fixture fixture(true);LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
            c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready() && c.safeMode();}));
            QVERIFY(c.actionBlocked()["exitSafeMode"].toString().isEmpty());
            QCOMPARE(c.loops()->rowCount()>=0,true);QVERIFY(c.ownedSubscriptions().empty());
            c.exitSafeMode();QVERIFY(waitFor([&]{return c.ready() && !c.safeMode() && !c.reconnecting();},15000));
            QCOMPARE(c.rebootNotice()["cause"].toString(),"Exit safe mode");
            c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        }
        Fixture fixture;LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready();}));
        QVERIFY(c.actionBlocked()["exitSafeMode"].toString().contains("not in safe mode"));
        c.rename("doomed");QVERIFY(waitFor([&]{return c.deviceName()=="doomed" && !c.configBusy();}));
        c.save();QVERIFY(waitFor([&]{return !c.unsaved() && !c.configBusy();}));
        c.factoryReset();QVERIFY(waitFor([&]{return c.ready() && !c.reconnecting() && c.rebootNotice()["visible"].toBool();},15000));
        QCOMPARE(c.rebootNotice()["cause"].toString(),"Factory reset");QVERIFY(c.deviceName().isEmpty());
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void reconnectWindowAndDisconnect() {
        {
            // Disconnect during the reconnect: nothing reopens afterwards.
            Fixture fixture;LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
            c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready();}));
            c.reboot();QVERIFY(waitFor([&]{return c.reconnecting();},5000));
            c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.reconnecting() && !c.linked();}));
            QTest::qWait(1500);QVERIFY(!c.linked());QVERIFY(!c.loading());QVERIFY(!c.linkLost());
        }
        // A device that never returns: the window ends in the link-lost state.
        Fixture fixture;LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.setReconnectWindow(2000);
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready();}));
        c.reboot();QVERIFY(waitFor([&]{return c.reconnecting();},5000));
        fixture.process.kill();fixture.process.waitForFinished(5000);
        QVERIFY(waitFor([&]{return !c.reconnecting() && c.linkLost();},10000));
        QVERIFY(c.lastError()["message"].toString().contains("did not reconnect"));
        QVERIFY(c.rebootNotice().isEmpty());QVERIFY(c.actionBlocked()["reboot"].toString()!="The device is rebooting");
        c.disconnectDevice();QVERIFY(!c.linkLost());
    }
    void retryAfterLinkLoss() {
        Fixture fixture;LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready();}));
        QVERIFY(waitFor([&]{return !c.ownedSubscriptions().empty();}));
        const auto subscriptions=c.ownedSubscriptions().size();
        const auto id=c.deviceId();
        QTest::qWait(1200); // let uptime advance past the reconnect margin
        fixture.command("stall");QVERIFY(waitFor([&]{return c.linkLost();},6000));
        QCOMPARE(c.deviceId(),id); // the overlay keeps naming the device
        QVERIFY(!c.reconnecting());QVERIFY(c.rebootNotice().isEmpty());
        fixture.command("resume");c.retry();QVERIFY(waitFor([&]{return c.ready();}));
        QVERIFY(c.rebootNotice().isEmpty()); // uptime kept growing: no reboot happened
        // The stalled link left its statistics groups behind; Retry releases
        // exactly those before taking slots again.
        QVERIFY(waitFor([&]{return c.ownedSubscriptions().size()==subscriptions;}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        {
            Session s(fixture.endpoint.toStdString());Client raw(s);raw.info();
            for(const auto &g:raw.groups().groups) {if(g.kind==GroupKind::Subscription) {QVERIFY(!g.configured);}}
        }
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready();}));
        fixture.command("reboot");QVERIFY(waitFor([&]{return c.linkLost();},6000));
        QVERIFY(!c.reconnecting()); // an unrequested loss waits for the user
        c.retry();QVERIFY(waitFor([&]{return c.ready();},10000));
        QVERIFY(c.rebootNotice()["visible"].toBool());QVERIFY(!c.rebootNotice()["expected"].toBool());
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        QVERIFY(!c.linkLost());
        // Disconnect from the overlay clears the lost device.
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready();}));
        fixture.command("stall");QVERIFY(waitFor([&]{return c.linkLost();},6000));
        c.disconnectDevice();QVERIFY(!c.linkLost());QVERIFY(c.deviceId().isEmpty());
        fixture.command("resume");
    }
    void connectArmFaultAndCleanup() {
        Fixture fixture;LocalConnection connection;
        bool wrongThread=false;
        connect(&connection,&Connection::changed,&connection,[&]{wrongThread|=QThread::currentThread()!=connection.thread();});
        connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled();}));
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        QTest::qWait(1300);QCOMPARE(connection.armState(),1); // automatic keepalives
        const auto stats=fixture.stats();QVERIFY(stats.contains("values 0\n"));QVERIFY(!stats.contains("prefix 0\n"));
        fixture.command("fault");QVERIFY(waitFor([&]{return connection.armState()==2;}));
        // An active disarming condition refuses ACK_FAULT; the GUI says so
        // instead of sending a request the device must refuse.
        QVERIFY(waitFor([&]{return !connection.ackEnabled() && connection.ackBlockedReason().contains("still active");}));
        QCOMPARE(connection.activeFaults(),1);QVERIFY(connection.faultHistory()>=1);
        connection.acknowledge();QTest::qWait(150);QCOMPARE(connection.armState(),2);
        fixture.command("fault-clear");QVERIFY(waitFor([&]{return connection.ackEnabled();}));
        connection.acknowledge();
        QVERIFY(waitFor([&]{return connection.armState()==0;}));
        QCOMPARE(connection.activeFaults(),0);
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing() && !connection.linked();}));
        QVERIFY(!wrongThread);
        Session s(fixture.endpoint.toStdString());Client c(s);c.info();
        const auto groups=c.groups();for(const auto &g:groups.groups) {QVERIFY(!g.configured);}
    }
    void disarmRacesDelayedArm() {
        Fixture fixture;LocalConnection connection;connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled();}));
        fixture.command("slow 400");connection.arm();QTest::qWait(70);connection.disarm();
        QVERIFY(waitFor([&]{return connection.ready() && !connection.arming() && connection.armEnabled();},10000));
        QTest::qWait(600);QCOMPARE(connection.armState(),0);
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
    void laterDisarmSurvivesFirstTimeout() {
        Fixture fixture;LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled();}));c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        fixture.command("drop-disarm 1");c.disarm();QTest::qWait(50);c.disarm();
        QVERIFY(waitFor([&]{return c.armState()==0 && c.armEnabled();},6000));
        QCOMPARE(c.lastError()["kind"].toInt(),int(ErrorKind::Timeout));
        QVERIFY(fixture.stats().contains("disarms 2\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void disarmBeforeSessionPublication() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();
         const auto g=client.cmdConfigure(0,0,{});client.arm();client.command(0,g.epoch,0);}
        LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(!c.linked()); // opening is deferred to the next GUI turn
        c.disarm();QVERIFY(waitFor([&]{return c.armEnabled();}));
        QCOMPARE(c.armState(),0);QCOMPARE(c.reason(),"Explicit disarm");
        QVERIFY(fixture.stats().contains("disarms 1\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void closeAfterUncertainArm() {
        Fixture fixture;LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled();}));fixture.command("drop-arm-reply 1");
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();},8000));
        QVERIFY(!fixture.stats().contains("group "));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        QCOMPARE(c.armState(),0);c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void observerCloseWaitsForExplicitDisarm() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();
         const auto g=client.cmdConfigure(0,0,{});client.arm();client.command(0,g.epoch,0);}
        fixture.command("slow 100");fixture.command("drop-disarm 1");
        LocalConnection c;c.connectTo(fixture.endpoint);c.disarm();
        QVERIFY(waitFor([&]{return c.linked();}));
        QVERIFY(fixture.stats().contains("disarms 1\n"));
        QVERIFY(c.loading());c.disconnectDevice();QVERIFY(c.closing());QVERIFY(c.linked());
        QVERIFY(waitFor([&]{return !c.closing() && !c.linked();},6000));
        QCOMPARE(c.lastError()["kind"].toInt(),int(ErrorKind::Timeout));
        const auto stats=fixture.stats();QVERIFY(stats.contains("disarms 1\n"));
        QVERIFY(stats.contains("group 0 0 0\n"));QVERIFY(!stats.contains("group 1 "));
    }
    void multipleLoopsAndNoValueWrites() {
        Fixture fixture;fixture.multiLoop();LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled();}));c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        QTest::qWait(1200);const auto stats=fixture.stats();QVERIFY(stats.contains("values 0\n"));
        QVERIFY(stats.contains("group 0 0 "));QVERIFY(stats.contains("group 1 2 "));
        QCOMPARE(c.armState(),1);c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        QVERIFY(!fixture.stats().contains("group "));
    }
    void existingInputAndUncertainPartialSetup() {
        {Fixture fixture;fixture.multiLoop();
         {Session s(fixture.endpoint.toStdString());Client c(s);c.info();
          const auto items=c.items(112);const auto input=std::find_if(items.begin(),items.end(),[](const auto &v){return v.kind==ItemKind::Input;});
          QVERIFY(input!=items.end());const Ref ref{112,0,input->id};c.cmdConfigure(0,0,{&ref,1});}
         LocalConnection c;c.connectTo(fixture.endpoint);
         QVERIFY(waitFor([&]{return c.ready() && !c.lastError().isEmpty();}));QVERIFY(!c.armEnabled());
         c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
         const auto stats=fixture.stats();QVERIFY(stats.contains("group 0 0 1\n"));QVERIFY(!stats.contains("group 1 "));}
        Fixture fixture;fixture.multiLoop();fixture.command("drop-config-reply 2");
        LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.ready() && c.lastError()["outcomeUnknown"].toBool();}));QVERIFY(!c.armEnabled());
        QVERIFY(!fixture.stats().contains("group "));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void observeAlreadyArmed() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client c(s);c.info();auto g=c.cmdConfigure(0,0,{});
         c.command(0,g.epoch,0);c.arm();c.command(0,g.epoch,1);}
        LocalConnection connection;connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.ready() && connection.snapshot()->status.has_value();}));
        QCOMPARE(connection.armState(),1);QVERIFY(!connection.armEnabled());
        // Observing must not send keepalives for the old owner's group.
        QVERIFY(waitFor([&]{return connection.armState()==0 && connection.reason()=="Command watchdog";},3000));
        QVERIFY(!connection.armEnabled());
        const auto stats=fixture.stats();QVERIFY(!stats.contains("group 1 "));
        connection.disarm();QVERIFY(waitFor([&]{return connection.armEnabled();}));
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
        Session s(fixture.endpoint.toStdString());Client c(s);c.info();const auto groups=c.groups();
        QVERIFY(groups.groups.at(groups.subscription_capacity).configured);
        for(const auto &g:groups.groups) {if(g.kind==GroupKind::Command && g.id!=0) {QVERIFY(!g.configured);}}
    }
    void safeModeAndLostLink() {
        qInfo("safeModeAndLostLink: safe-mode connection");
        {Fixture fixture(true);LocalConnection c;c.connectTo(fixture.endpoint);
         QVERIFY(waitFor([&]{return c.ready();}));QVERIFY(c.safeMode());QVERIFY(!c.armEnabled());
         c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));}
        qInfo("safeModeAndLostLink: ordinary connection");
        Fixture fixture;LocalConnection c;c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        qInfo("safeModeAndLostLink: stall and await link-loss teardown");
        fixture.command("stall");QVERIFY(waitFor([&]{return !c.linked();},6000));QVERIFY(!c.armEnabled());
        qInfo("safeModeAndLostLink: resume and reconnect");
        fixture.command("resume");c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        qInfo("safeModeAndLostLink: reboot and await link-loss teardown");
        fixture.command("reboot");QVERIFY(waitFor([&]{return !c.linked();}));
        qInfo("safeModeAndLostLink: reconnect after reboot");
        c.connectTo(fixture.endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled();}),qPrintable(c.armBlockedReason()+": "+c.lastError()["message"].toString()+
            QString(" (%1/%2)").arg(c.lastError()["result"].toInt()).arg(c.lastError()["detail"].toInt())));
        QCOMPARE(c.armState(),0);
        for(const auto &loop:c.snapshot()->info.loops) {QCOMPARE(loop.rate_hz,uint32_t(10));}
        qInfo("safeModeAndLostLink: explicit disconnect");
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        qInfo("safeModeAndLostLink: final destruction");
    }
    void observeDuringSlowDiscovery() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();
         const auto g=client.cmdConfigure(0,0,{});client.arm();client.command(0,g.epoch,0);}
        fixture.command("slow 50");LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.ready() && c.snapshot()->status.has_value();},15000));
        QVERIFY(waitFor([&]{return c.armState()==0 && c.reason()=="Command watchdog";}));
        QVERIFY(!c.armEnabled());QVERIFY(c.armBlockedReason().contains("DISARM"));
        QVERIFY(!fixture.stats().contains("group 1 "));
        c.disarm();QVERIFY(waitFor([&]{return c.armEnabled();}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void disconnectDuringDiscovery() {
        Fixture fixture;fixture.command("slow 500");LocalConnection c;
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.linked();}));
        c.disarm();c.disconnectDevice();QVERIFY(waitFor([&]{return !c.linked() && !c.closing();}));
        QTest::qWait(600);QVERIFY(!c.ready());
        fixture.command("slow 0");c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
#ifndef Q_OS_WIN
    // Same-link update (as on a serial port): the link stays open, the GUI
    // waits for Boot, then checks and confirms on the reloaded model.
    void firmwareUpdateBootInPlace() {
        Fixture fixture(false,true);QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled() && c.firmware()["known"].toBool();}));
        const auto start=c.otaStatus()->running_slot;
        c.inspectImage(writeImage(dir,"p.bin",71,"7.0.0"));c.startUpdate(true);
        bool reconnected=false;
        QVERIFY2(waitFor([&]{reconnected|=c.reconnecting();return c.ota()["phase"]=="confirmed";},30000),qPrintable(otaText(c)));
        QVERIFY(!reconnected);QVERIFY(c.otaStatus()->running_slot!=start);QVERIFY(!c.otaStatus()->pending_confirm);
        QVERIFY(fixture.stats().contains("reboots 1\n"));
        c.closeUpdate();c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void bootDuringCommandSetup() {
        Fixture fixture(false,true);fixture.command("drop-config-reply 1");
        LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.ready();}));const auto generation=c.snapshot()->generation;
        QVERIFY(waitFor([&]{return fixture.stats().contains("group 0 ");}));
        fixture.command("reboot");
        QVERIFY(waitFor([&]{return c.ready() && c.snapshot()->generation!=generation && c.armEnabled();},15000));
        QCOMPARE(c.armState(),0);QVERIFY(fixture.stats().contains("values 0\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
#endif
    void otaViewModel() {
        OtaJob job;job.image=std::make_shared<const OtaImage>(describeOtaImage(otaFixture(4096,3)));
        job.fileName="a.bin";job.previousVersion="1.0.0";job.deviceId="02444c000001";
        job.phase=OtaJob::Phase::Writing;job.offset=2048;job.retries=2;
        auto view=otaView(job,"1.0.0");auto steps=view["steps"].toList();
        QCOMPARE(view["phase"].toString(),QString("writing"));QCOMPARE(steps.size(),6);
        QCOMPARE(steps[1].toMap()["state"].toString(),QString("active"));
        QVERIFY(steps[1].toMap()["detail"].toString().contains("2 chunks retried"));
        QVERIFY(view["canAbort"].toBool());QVERIFY(!view["canClose"].toBool());QCOMPARE(view["percent"].toInt(),49);
        job.phase=OtaJob::Phase::Unconfirmed;view=otaView(job,{});steps=view["steps"].toList();
        QVERIFY(view["canConfirm"].toBool() && view["canRollBack"].toBool() && view["canClose"].toBool());
        QCOMPARE(steps[4].toMap()["state"].toString(),QString("pass"));QCOMPARE(steps[5].toMap()["state"].toString(),QString("pending"));
        job.phase=OtaJob::Phase::RolledBack;job.resetReason="Task watchdog";view=otaView(job,{});steps=view["steps"].toList();
        QCOMPARE(steps[3].toMap()["state"].toString(),QString("pass"));
        QCOMPARE(steps[4].toMap()["state"].toString(),QString("fail"));QCOMPARE(steps[5].toMap()["state"].toString(),QString("fail"));
        QCOMPARE(view["tone"].toString(),QString("fault"));QVERIFY(view["canCopy"].toBool());
        QVERIFY(view["explanation"].toString().contains("Task watchdog"));
        job.rollback=true;view=otaView(job,{});steps=view["steps"].toList();   // the user's roll back
        QCOMPARE(steps[4].toMap()["state"].toString(),QString("pass"));QCOMPARE(steps[5].toMap()["state"].toString(),QString("pending"));
        QCOMPARE(view["tone"].toString(),QString("pending"));QVERIFY(view["explanation"].toString().isEmpty());job.rollback=false;
        job.phase=OtaJob::Phase::Writing;job.failedAt=OtaJob::Phase::Writing;
        job.phase=OtaJob::Phase::Failed;job.failedAt=OtaJob::Phase::Rebooting;job.message="refused";
        view=otaView(job,{});steps=view["steps"].toList();
        QCOMPARE(steps[2].toMap()["state"].toString(),QString("pass"));QCOMPARE(steps[3].toMap()["state"].toString(),QString("fail"));
        QCOMPARE(view["note"].toString(),QString("refused"));
        FirmwareFacts facts;facts.version="1.0.0";OtaStatus status;status.running_slot=1;status.boot_slot=1;
        status.pending_confirm=true;status.capacity=0x400000;facts.status=status;
        auto card=firmwareView(facts);
        QVERIFY(card["canConfirm"].toBool());QVERIFY(!card["canAbort"].toBool());
        QVERIFY(card["note"].toString().contains("did not install"));QVERIFY(card["running"].toString().contains("not confirmed"));
        facts.knownCandidate=true;QVERIFY(!firmwareView(facts)["note"].toString().contains("did not install"));
        status.pending_confirm=false;status.state=2;status.size=4096;facts.status=status;card=firmwareView(facts);
        QVERIFY(card["canAbort"].toBool());QVERIFY(card["state"].toString().contains("prepared"));
        status.boot_slot=0;facts.status=status;QVERIFY(!firmwareView(facts)["canAbort"].toBool()); // already committed
    }
    // Upload, one REBOOT, reconnect, identity check and one CONFIRM; then an
    // unconfirmed install confirmed explicitly, and one rolled back.
    void firmwareUpdate() {
        Fixture fixture;QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled() && c.firmware()["known"].toBool();}));
        const auto start=*c.otaStatus();QCOMPARE(start.state,uint8_t(0));QVERIFY(!start.pending_confirm);
        QVERIFY(!c.inspectImage(dir.filePath("missing.bin"))["ok"].toBool());
        const auto a=writeImage(dir,"a.bin",21,"2.1.0");
        const auto info=c.inspectImage(QUrl::fromLocalFile(a).toString());
        QVERIFY2(info["ok"].toBool(),qPrintable(info["problem"].toString()));QCOMPARE(info["version"].toString(),QString("2.1.0"));
        c.startUpdate(true);
        QVERIFY(c.ota()["active"].toBool());QVERIFY(!c.armEnabled());QVERIFY(c.armBlockedReason().contains("firmware update"));
        QVERIFY(c.actionBlocked()["save"].toString().contains("firmware update"));
        QVERIFY(!c.updateBlocked().isEmpty());
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="confirmed";},30000),qPrintable(otaText(c)));
        QVERIFY(c.ready());QVERIFY(!c.otaStatus()->pending_confirm);QVERIFY(c.otaStatus()->running_slot!=start.running_slot);
        QCOMPARE(QString::fromStdString(c.snapshot()->info.build_id),info["buildId"].toString());
        QCOMPARE(c.rebootNotice()["cause"].toString(),QString("Firmware update"));
        QVERIFY(c.firmware()["lastUpdate"].toString().contains("2.1.0"));
        QVERIFY(fixture.stats().contains("reboots 1\n"));
        c.closeUpdate();QVERIFY(c.ota().isEmpty());c.dismissRebootNotice();
        QVERIFY2(waitFor([&]{return c.armEnabled();}),qPrintable(c.armBlockedReason()));
        // Installed without confirming: pending, ARM refused by the device;
        // the user's Confirm sends it once after the identity check.
        const auto confirmedSlot=c.otaStatus()->running_slot;
        c.inspectImage(writeImage(dir,"b.bin",22,"2.2.0"));c.startUpdate(false);
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="unconfirmed";},30000),qPrintable(otaText(c)));
        QVERIFY(c.otaStatus()->pending_confirm);QVERIFY(c.firmware()["canConfirm"].toBool());
        QVERIFY(waitFor([&]{return !c.armEnabled();}));QVERIFY(fixture.stats().contains("reboots 2\n"));
        QVERIFY(!c.updateBlocked().isEmpty());
        c.confirmFirmware();
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="confirmed";},10000),qPrintable(otaText(c)));
        QVERIFY(!c.otaStatus()->pending_confirm);QVERIFY(c.otaStatus()->running_slot!=confirmedSlot);
        c.closeUpdate();c.dismissRebootNotice();QVERIFY(waitFor([&]{return c.armEnabled();}));
        // Installed without confirming, then rolled back with one REBOOT.
        const auto keptSlot=c.otaStatus()->running_slot;const auto keptBuild=c.snapshot()->info.build_id;
        c.inspectImage(writeImage(dir,"c.bin",23,"9.9.9"));c.startUpdate(false);
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="unconfirmed";},30000),qPrintable(otaText(c)));
        c.rollBackFirmware();
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="rolledBack";},30000),qPrintable(otaText(c)));
        QCOMPARE(c.otaStatus()->running_slot,keptSlot);QVERIFY(!c.otaStatus()->pending_confirm);
        QCOMPARE(c.snapshot()->info.build_id,keptBuild);
        QCOMPARE(c.rebootNotice()["cause"].toString(),QString("Roll back firmware update"));
        QVERIFY(c.firmware()["lastUpdate"].toString().contains("rolled back"));
        QVERIFY(fixture.stats().contains("reboots 4\n"));
        c.closeUpdate();c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    // Abort while writing (DISARM stays live), and a lost REBOOT that is
    // never repeated: the prepared image waits and blocks plain reboots.
    void firmwareUpdateAbortAndLostReboot() {
        Fixture fixture;QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled() && c.firmware()["known"].toBool();}));
        c.inspectImage(writeImage(dir,"big.bin",31,"3.0.0",256*1024));c.setOtaPauseAt(40);c.startUpdate(true);
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="writing" && c.ota()["percent"].toInt()>=40;},15000),qPrintable(otaText(c)));
        QVERIFY(c.ota()["active"].toBool());QVERIFY(!c.reason().startsWith("Firmware update"));
        const auto disarms=fixture.stats();c.disarm();
        QVERIFY(waitFor([&]{return fixture.stats()!=disarms;}));     // fast DISARM bypasses the busy service queue
        c.abortUpdate();QVERIFY(!c.ota()["canAbort"].toBool());
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="aborted";},10000),qPrintable(otaText(c)));
        QVERIFY(waitFor([&]{return c.otaStatus() && c.otaStatus()->state==0;}));
        c.setOtaPauseAt(-1);c.closeUpdate();
        QVERIFY(fixture.stats().contains("reboots 0\n"));
        // No reply and no reboot: wait out the window; REBOOT is not sent again.
        c.setReconnectWindow(3000);fixture.command("drop-reboot 1");
        c.inspectImage(writeImage(dir,"small.bin",32,"3.0.1"));c.startUpdate(true);
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="failed";},20000),qPrintable(otaText(c)));
        QVERIFY(c.ota()["note"].toString().contains("did not reboot"));
        QVERIFY(fixture.stats().contains("reboots 1\n"));
        QVERIFY(waitFor([&]{return c.otaStatus() && c.otaStatus()->state==2;}));
        QVERIFY(c.actionBlocked()["reboot"].toString().contains("prepared"));
        QVERIFY(c.firmware()["canAbort"].toBool());
        c.closeUpdate();c.abortPreparedUpdate();
        QVERIFY(waitFor([&]{return c.otaStatus() && c.otaStatus()->state==0 && !c.configBusy();}));
        QVERIFY(c.actionBlocked()["reboot"].toString().isEmpty());
        QVERIFY(fixture.stats().contains("reboots 1\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    // Stop and disconnect while the device has not come back: the update
    // ends as failed instead of waiting for a link that will not open.
    void firmwareUpdateDisconnectWhileReconnecting() {
        Fixture fixture;QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled() && c.firmware()["known"].toBool();}));
        c.inspectImage(writeImage(dir,"r.bin",61,"6.0.0"));c.startUpdate(true);
        QVERIFY2(waitFor([&]{return c.reconnecting();},15000),qPrintable(otaText(c)));
        fixture.process.kill();fixture.process.waitForFinished(5000);
        QVERIFY(waitFor([&]{return c.reconnecting() && !c.linked();}));
        c.disconnectDevice();
        QVERIFY2(waitFor([&]{return c.ota()["phase"]=="failed" && !c.closing() && !c.reconnecting();}),qPrintable(otaText(c)));
        QVERIFY(c.ota()["note"].toString().contains("unconfirmed"));QVERIFY(c.ota()["canClose"].toBool());
        c.closeUpdate();QVERIFY(c.ota().isEmpty());
    }
    // A prepared image and an unconfirmed build that another client left.
    void foreignOtaStates() {
        Fixture fixture;QTemporaryDir dir;
        const auto image=loadOtaImage(writeImage(dir,"x.bin",41,"4.0.0").toStdString());
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();QCOMPARE(client.otaUpload(image.bytes).state,uint8_t(2));}
        LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready() && c.firmware()["known"].toBool();}));
        QCOMPARE(c.otaStatus()->state,uint8_t(2));QVERIFY(c.firmware()["canAbort"].toBool());
        QVERIFY(c.actionBlocked()["reboot"].toString().contains("prepared"));
        QVERIFY(c.actionBlocked()["saveAndReboot"].toString().contains("prepared"));
        QVERIFY(c.updateBlocked().contains("unfinished"));
        c.abortPreparedUpdate();QVERIFY(waitFor([&]{return c.otaStatus() && c.otaStatus()->state==0 && !c.configBusy();}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
        OtaUpdateOptions options;options.confirm=false;
        const auto installed=otaUpdate(fixture.endpoint.toStdString(),SessionOptions{},image,options);
        QVERIFY(installed.rebooted);QVERIFY(!installed.confirmed);
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.ready() && c.firmware()["known"].toBool();}));
        QVERIFY(c.otaStatus()->pending_confirm);QVERIFY(c.firmware()["note"].toString().contains("did not install"));
        QVERIFY(waitFor([&]{return !c.armEnabled();}));
        c.confirmFirmware();
        QVERIFY(waitFor([&]{return c.otaStatus() && !c.otaStatus()->pending_confirm && !c.configBusy();},10000));
        QVERIFY(c.ota().isEmpty());QVERIFY(waitFor([&]{return c.armEnabled();}));
        QVERIFY(fixture.stats().contains("reboots 1\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    // The H2b-2 bare-board OTA check, skipped unless both
    // DRIVELINK_GUI_BENCH_ENDPOINT and DRIVELINK_GUI_BENCH_OTA_IMAGE are set.
    // Installs the image through the GUI backend and confirms it; with
    // DRIVELINK_GUI_BENCH_OTA_RESTORE it then installs that image (the
    // board's original application) the same way.
    void benchOta() {
        const auto endpoint=qEnvironmentVariable("DRIVELINK_GUI_BENCH_ENDPOINT");
        const auto candidate=qEnvironmentVariable("DRIVELINK_GUI_BENCH_OTA_IMAGE");
        if(endpoint.isEmpty() || candidate.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_ENDPOINT and DRIVELINK_GUI_BENCH_OTA_IMAGE");}
        const auto restore=qEnvironmentVariable("DRIVELINK_GUI_BENCH_OTA_RESTORE");
        QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(endpoint);
        QVERIFY2(waitFor([&]{return c.ready() && c.firmware()["known"].toBool();},30000),qPrintable(c.lastError()["message"].toString()));
        QVERIFY2(!c.unsaved() && !c.pendingReboot(),"refusing to run with unsaved or pending changes");
        QVERIFY2(c.updateBlocked().isEmpty(),qPrintable(c.updateBlocked()));
        const auto name=c.deviceName();const auto layout=c.snapshot()->configured.size();
        auto install=[&](const QString &path) {
            const auto before=*c.otaStatus();
            const auto info=c.inspectImage(path);
            QVERIFY2(info["ok"].toBool(),qPrintable(info["problem"].toString()));
            qInfo("bench OTA: %s %s build %s from slot %d",qPrintable(info["fileName"].toString()),qPrintable(info["version"].toString()),
                  qPrintable(info["buildId"].toString()),before.running_slot);
            QElapsedTimer timer;timer.start();c.startUpdate(true);
            QVERIFY2(waitFor([&]{return c.ota()["active"].toBool() && c.ota()["phase"]=="writing";},10000),qPrintable(otaText(c)));
            QVERIFY(!c.armEnabled());
            QVERIFY2(waitFor([&]{return !c.ota()["active"].toBool();},240000),qPrintable(otaText(c)));
            const auto ota=c.ota();
            for(const auto &step:ota["steps"].toList()) {
                qInfo("bench OTA:   %s [%s] %s",qPrintable(step.toMap()["label"].toString()),qPrintable(step.toMap()["state"].toString()),
                      qPrintable(step.toMap()["detail"].toString()));
            }
            QVERIFY2(ota["phase"]=="confirmed",qPrintable(otaText(c)));
            QVERIFY(waitFor([&]{return c.otaStatus().has_value();}));
            const auto after=*c.otaStatus();
            qInfo("bench OTA: confirmed in %.1f s, slot %d -> %d, build %s, reset reason '%s'",timer.elapsed()/1000.0,before.running_slot,
                  after.running_slot,c.snapshot()->info.build_id.c_str(),qPrintable(c.rebootNotice()["reason"].toString()));
            QVERIFY(after.running_slot!=before.running_slot);QVERIFY(!after.pending_confirm);QCOMPARE(after.state,uint8_t(0));
            QCOMPARE(QString::fromStdString(c.snapshot()->info.build_id),info["buildId"].toString());
            QCOMPARE(c.rebootNotice()["cause"].toString(),QString("Firmware update"));
            QVERIFY(c.rebootNotice()["lost"].toStringList().isEmpty());
            c.closeUpdate();c.dismissRebootNotice();
            QCOMPARE(c.deviceName(),name);QCOMPARE(c.snapshot()->configured.size(),layout);
            QVERIFY2(waitFor([&]{return c.armEnabled();},15000),qPrintable(c.armBlockedReason()));
        };
        install(candidate);
        if(QTest::currentTestFailed()) {return;}
        if(!restore.isEmpty()) {install(restore);}
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
    }
    // The H2b bare-board check, skipped unless DRIVELINK_GUI_BENCH_ENDPOINT
    // names a device (serial://COM3, or a vdev tcp:// endpoint for a dry run).
    // Runs on the normal firmware: a temporary mock actuator raises the fault,
    // and the layout and name are restored afterwards.
    void benchBoard() {
        const auto endpoint=qEnvironmentVariable("DRIVELINK_GUI_BENCH_ENDPOINT");
        if(endpoint.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_ENDPOINT to run on a device");}
        constexpr uint8_t kMockActuator=112; // actuator.mock, as the fixtures use it
        QTemporaryDir dir;
        auto open=[&](LocalConnection &c) {
            c.setSettingsPath(dir.filePath("gui.ini"));c.connectTo(endpoint);
            return waitFor([&]{return c.ready() && c.snapshot()->status.has_value();},30000);
        };
        LocalConnection c;
        QVERIFY2(open(c),qPrintable(c.lastError()["message"].toString()));
        QVERIFY2(!c.unsaved() && !c.pendingReboot(),"refusing to run with unsaved or pending changes");
        const auto original=c.deviceName();
        qInfo("bench: device %s firmware %s name '%s'",qPrintable(c.deviceId()),qPrintable(c.info()["firmware"].toString()),qPrintable(original));
        if(endpoint.startsWith("serial://")) {
            const auto ports=c.serialPorts();
            QVERIFY(std::any_of(ports.begin(),ports.end(),[&](const QVariant &p){return p.toMap()["endpoint"]==endpoint && p.toMap()["inUse"].toBool();}));
        }
        QCOMPARE(c.recentDevices()[0].toMap()["id"].toString(),c.deviceId());
        QVERIFY(waitFor([&]{
            for(int i=0;i<c.loops()->rowCount();++i) {
                const auto row=c.loops()->index(i,0);
                if(row.data(LoopTable::Running).toBool() && !row.data(LoopTable::HasRunTime).toBool()) {return false;}
            }
            return c.loops()->rowCount()>0;
        },10000));
        for(int i=0;i<c.loops()->rowCount();++i) {
            const auto row=c.loops()->index(i,0);
            qInfo("bench: loop %d %u Hz run %u us max %u us budget %.3f overruns %s",row.data(LoopTable::LoopId).toInt(),
                row.data(LoopTable::RateHz).toUInt(),row.data(LoopTable::LastRunUs).toUInt(),row.data(LoopTable::MaxRunUs).toUInt(),
                row.data(LoopTable::Budget).toDouble(),qPrintable(row.data(LoopTable::Overruns).toString()));
        }
        // Rename, SAVE, REBOOT and read the name back after the reboot.
        c.rename("h2b-bench");QVERIFY(waitFor([&]{return c.deviceName()=="h2b-bench" && c.unsaved() && !c.configBusy();}));
        c.save();QVERIFY(waitFor([&]{return !c.unsaved() && !c.configBusy();},10000));
        c.reboot();
        QVERIFY2(waitFor([&]{return c.ready() && c.rebootNotice()["visible"].toBool();},30000),qPrintable(c.lastError()["message"].toString()));
        qInfo("bench: reboot notice reason '%s' expected %d",qPrintable(c.rebootNotice()["reason"].toString()),c.rebootNotice()["expected"].toBool());
        QVERIFY(c.rebootNotice()["expected"].toBool());QVERIFY(c.rebootNotice()["lost"].toStringList().isEmpty());
        QCOMPARE(c.deviceName(),QString("h2b-bench"));c.dismissRebootNotice();
        // Arm with prefix-only keepalives, hold, disarm.
        QVERIFY2(waitFor([&]{return c.armEnabled();},15000),qPrintable(c.armBlockedReason()));
        c.arm();QVERIFY2(waitFor([&]{return c.armState()==1;}),qPrintable(c.lastError()["message"].toString()+" / "+c.reason()));
        QVERIFY(c.actionBlocked()["save"].toString().contains("armed"));
        QTest::qWait(1500);QCOMPARE(c.armState(),1);
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0 && c.armEnabled();}));
        // A real disarming fault from a mock actuator (normal firmware):
        // ACK is refused while active, allowed once cleared.
        auto device=[&](std::function<void(Client &)> operation) {
            QString error="pending";
            if(!c.runOnDevice(std::move(operation),[&](QString e){error=e;})) {return QString("no session");}
            waitFor([&]{return error!="pending";},10000);
            return error;
        };
        const uint8_t loop=c.snapshot()->info.loops.at(0).id;uint8_t mock=0;
        QCOMPARE(device([&](Client &client){mock=client.add(kMockActuator,"h2b_mock",loop).instance;}),QString());
        QVERIFY(waitFor([&]{return c.pendingReboot();}));
        c.saveAndReboot();
        QVERIFY2(waitFor([&]{return c.ready() && c.rebootNotice()["visible"].toBool();},30000),qPrintable(c.lastError()["message"].toString()));
        QCOMPARE(c.rebootNotice()["cause"].toString(),QString("Save and reboot"));c.dismissRebootNotice();
        QVERIFY2(waitFor([&]{return c.armEnabled();},15000),qPrintable(c.armBlockedReason()));
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        const Ref diagnostic{kMockActuator,mock,770};
        QCOMPARE(device([&](Client &client){client.action(diagnostic,{1,1,0,0});}),QString());
        QVERIFY(waitFor([&]{return c.armState()==2 && c.activeFaults()>=1;}));
        QVERIFY(waitFor([&]{return !c.ackEnabled() && c.ackBlockedReason().contains("still active");}));
        QCOMPARE(device([&](Client &client){client.action(diagnostic,{1,0,0,0});}),QString());
        QVERIFY(waitFor([&]{return c.ackEnabled();},10000));
        c.acknowledge();QVERIFY(waitFor([&]{return c.armState()==0;}));QCOMPARE(c.activeFaults(),0);
        qInfo("bench: fault history %d after acknowledge",c.faultHistory());
        // Remove the mock again: the layout returns to what the board had.
        QCOMPARE(device([&](Client &client){client.remove(kMockActuator,mock);}),QString());
        QVERIFY(waitFor([&]{return c.pendingReboot();}));
        c.saveAndReboot();
        QVERIFY(waitFor([&]{return c.ready() && c.rebootNotice()["visible"].toBool();},30000));c.dismissRebootNotice();
        QVERIFY(!c.snapshot()->configured.contains({kMockActuator,mock}));
        // Restore the name the board had.
        c.rename(original);QVERIFY(waitFor([&]{return c.deviceName()==original && !c.configBusy();}));
        c.save();QVERIFY(waitFor([&]{return !c.unsaved() && !c.configBusy();},10000));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
        Session s(endpoint.toStdString());Client raw(s);QCOMPARE(QString::fromStdString(raw.info().name),original);
        for(const auto &g:raw.groups().groups) {QVERIFY(!g.configured);}
    }
    // The OTA dialog in the real shell: it blocks the page like a dialog,
    // Install starts the update, DISARM in the status bar stays clickable.
    void otaDialogInShell() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);
        Fixture fixture;QTemporaryDir dir;LocalConnection connection;connection.setSettingsPath(dir.filePath("gui.ini"));
        SafetyKeys keys;connect(&keys,&SafetyKeys::escapeRequested,&connection,&Connection::disarm);
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.loadFromModule("DriveLink.Shell","Shell");QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());QVERIFY(window);
        connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled() && connection.firmware()["known"].toBool();}));
        auto *card=window->findChild<QQuickItem *>("chooseImageButton");QVERIFY(card);QVERIFY(card->isEnabled());
        auto *dialog=window->property("otaDialog").value<QQuickItem *>();QVERIFY(dialog);QVERIFY(!dialog->isVisible());
        dialog->setProperty("choice",connection.inspectImage(writeImage(dir,"s.bin",51,"5.0.0")));
        QVERIFY(waitFor([&]{return dialog->isVisible() && window->property("modalOpen").toBool();}));
        auto *scrim=window->findChild<QQuickItem *>("pageScrim");auto *bar=window->findChild<QQuickItem *>("statusBar");
        QVERIFY(scrim && bar && scrim->isVisible());QCOMPARE(scrim->mapToScene({0,0}).y(),bar->height());
        // Esc closes the choice (and disarms); nothing was started.
        QTest::keyClick(window,Qt::Key_Escape);QVERIFY(waitFor([&]{return !dialog->isVisible();}));
        QVERIFY(connection.ota().isEmpty());
        dialog->setProperty("choice",connection.inspectImage(writeImage(dir,"s.bin",51,"5.0.0")));
        QVERIFY(waitFor([&]{return dialog->isVisible();}));
        // Wait until the control can be clicked, then click it once.
        auto clickable=[&](const char *name) {
            auto *item=visualChild(window->contentItem(),name);
            return item && item->isVisible() && item->isEnabled()?item:nullptr;
        };
        auto click=[&](const char *name) {
            if(!waitFor([&]{return clickable(name)!=nullptr;})) {return false;}
            auto *item=clickable(name);QTest::qWait(50); // positioners lay out on polish
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene({item->width()/2,item->height()/2}).toPoint());
            return true;
        };
        QVERIFY(click("otaInstall"));
        QVERIFY(waitFor([&]{return connection.ota()["active"].toBool();}));
        const auto before=fixture.stats();
        QVERIFY(click("disarmButton"));QVERIFY(waitFor([&]{return fixture.stats()!=before;}));
        QVERIFY2(waitFor([&]{return connection.ota()["phase"]=="confirmed";},30000),qPrintable(otaText(connection)));
        QVERIFY(dialog->isVisible());
        // The reboot notice waits for the dialog, then shows.
        auto *notice=window->findChild<QQuickItem *>("rebootNotice");QVERIFY(notice);QVERIFY(!notice->isVisible());
        QVERIFY(click("otaClose"));
        QVERIFY(waitFor([&]{return !dialog->isVisible() && notice->isVisible();}));
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
    void keysAndShell() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);
        Fixture fixture;LocalConnection connection;SafetyKeys keys;
        connect(&keys,&SafetyKeys::escapeRequested,&connection,&Connection::disarm);
        QSignalSpy escapes(&keys,&SafetyKeys::escapeRequested);
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.loadFromModule("DriveLink.Shell","Shell");QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());QVERIFY(window);
        connection.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return connection.armEnabled();}));
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        QQmlComponent component(&engine);component.setData("import QtQuick\nimport QtQuick.Controls.Basic\nTextField {text: 'before'; width: 300; height: 40}",QUrl());
        std::unique_ptr<QObject> field(component.create());QVERIFY2(field,qPrintable(component.errorString()));
        auto *item=qobject_cast<QQuickItem *>(field.get());item->setParentItem(window->contentItem());item->forceActiveFocus();
        QVERIFY(waitFor([&]{return item->hasActiveFocus();}));field->setProperty("text","changed");
        QTest::keyClick(window,Qt::Key_Escape);QCOMPARE(escapes.count(),1);QCOMPARE(field->property("text").toString(),"before");
        QVERIFY(waitFor([&]{return connection.armState()==0 && connection.armEnabled();}));
        // A DISARM click takes no focus, so it never ends (and sends) an open
        // edit; the shell restores the text last committed, like Esc.
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        item->forceActiveFocus();field->setProperty("text","typed");keys.editCommitted();field->setProperty("text","more");
        auto *disarmButton=window->findChild<QQuickItem *>("disarmButton");QVERIFY(disarmButton);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,disarmButton->mapToScene({disarmButton->width()/2,disarmButton->height()/2}).toPoint());
        QVERIFY(waitFor([&]{return connection.armState()==0 && connection.armEnabled();}));
        QVERIFY(item->hasActiveFocus());QCOMPARE(field->property("text").toString(),"typed");
        QTest::keyClick(window,Qt::Key_4,Qt::ControlModifier);QCOMPARE(window->property("page").toInt(),3);
        QQmlComponent popupComponent(&engine);
        popupComponent.setData("import QtQuick\nimport QtQuick.Controls.Basic\nPopup {width: 320; height: 100; focus: true; contentItem: TextField {objectName: 'popupEdit'; text: 'original'; property int changedCommits: 0; onEditingFinished: if (text !== 'original') ++changedCommits}}",QUrl());
        std::unique_ptr<QObject> popup(popupComponent.create());QVERIFY2(popup,qPrintable(popupComponent.errorString()));
        popup->setProperty("parent",QVariant::fromValue(window->contentItem()));QMetaObject::invokeMethod(popup.get(),"open");
        QVERIFY(waitFor([&]{return popup->property("opened").toBool();}));
        auto *popupEdit=popup->findChild<QQuickItem *>("popupEdit");QVERIFY(popupEdit);popupEdit->forceActiveFocus();
        QVERIFY(waitFor([&]{return popupEdit->hasActiveFocus();}));popupEdit->setProperty("text","edited");
        QTest::keyClick(window,Qt::Key_Escape);QCOMPARE(escapes.count(),2);
        QVERIFY(waitFor([&]{return !popup->property("visible").toBool();}));
        QCOMPARE(popupEdit->property("text").toString(),"original");QCOMPARE(popupEdit->property("changedCommits").toInt(),0);
        QVERIFY(waitFor([&]{return connection.armEnabled();}));
        auto *dialog=window->findChild<QObject *>("shellDialog");QVERIFY(dialog);QMetaObject::invokeMethod(dialog,"open");
        QVERIFY(waitFor([&]{return dialog->property("opened").toBool();}));
        auto *scrim=window->findChild<QQuickItem *>("pageScrim");auto *bar=window->findChild<QQuickItem *>("statusBar");
        QVERIFY(scrim && bar);QCOMPARE(scrim->mapToScene({0,0}).y(),bar->height());
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        QTest::keyClick(window,Qt::Key_Escape);QCOMPARE(escapes.count(),3);
        QVERIFY(waitFor([&]{return !dialog->property("visible").toBool() && connection.armState()==0 && connection.armEnabled();}));
        QMetaObject::invokeMethod(dialog,"open");QVERIFY(waitFor([&]{return dialog->property("opened").toBool();}));
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        auto *button=window->findChild<QQuickItem *>("disarmButton");QVERIFY(button);
        const auto at=button->mapToScene({button->width()/2,button->height()/2});
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,at.toPoint());
        QVERIFY(waitFor([&]{return connection.armState()==0;}));
        QTest::keyClick(window,Qt::Key_Escape);
        for(int i=0;i<6;++i) {
            QVERIFY(QMetaObject::invokeMethod(window,"notify",Q_ARG(QVariant,QString("toast %1").arg(i)),Q_ARG(QVariant,i==5?2:0)));
        }
        QCOMPARE(window->property("toastCount").toInt(),4);
        QVERIFY(waitFor([&]{return window->property("toastCount").toInt()==1;},5000));
        auto *toast=visualChild(window->contentItem(),"shellToast");QVERIFY(toast);
        auto *close=visualChild(toast,"closeButton");QVERIFY(close);
        const auto closeAt=close->mapToScene({close->width()/2,close->height()/2});
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,closeAt.toPoint());
        QVERIFY(waitFor([&]{return window->property("toastCount").toInt()==0;}));
        const auto beforeLostRelease=escapes.count();
        QTest::keyPress(window,Qt::Key_Escape);
        QTest::keyPress(window,Qt::Key_Escape); // previous release was outside the app
        QTest::keyRelease(window,Qt::Key_Escape);
        QCOMPARE(escapes.count(),beforeLostRelease+2);
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
};
QTEST_MAIN(BackendTests)
#include "test_backend.moc"
