#pragma once
// Shared by the GUI backend and Monitor tests: waiting on the event loop,
// finding QML items by objectName, and an isolated vdev with a scenario
// control file.
#include "connection.hpp"
#include "register.hpp"
#include <drivelink/protocol/gen/type_system.hpp>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QtTest>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#endif

// The static DriveLink.Shell module's C++ types (PlotItem).
void qml_register_types_DriveLink_Shell();

namespace drivelink::gui::test {
inline void registerShellTypes() {
    static const bool registered=(qml_register_types_DriveLink_Shell(),drivelink::gui::registerModulePanels(),true);
    (void)registered;
}
namespace defs=drivelink::protocol::defs;
inline bool waitFor(const std::function<bool()> &condition,int timeout=8000) {
    QElapsedTimer elapsed;elapsed.start();
    while(!condition() && elapsed.elapsed()<timeout) {QCoreApplication::processEvents();QTest::qWait(10);}
    return condition();
}
inline QQuickItem *visualChild(QQuickItem *parent,const QString &name) {
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
    explicit Fixture(bool safe=false,bool pty=false,bool bench=false,int linkTrialMs=0) {
        QStringList args={"--port","0","--state",directory.path()+"/state","--scenario-file",directory.path()+"/control"};
        if(safe) {args<<"--safe-mode";}
        if(pty) {args<<"--pty";}
        if(bench) {args<<"--allow-crash";} // BENCH_STALL
        if(linkTrialMs>0) {args<<"--link-trial-ms"<<QString::number(linkTrialMs);}
        process.start(QString::fromUtf8(VDEV_PATH),args);
        try {
            if(!process.waitForStarted(10000)) {throw std::runtime_error("vdev did not start");}
            QElapsedTimer ready;ready.start();
            while(!process.canReadLine() && ready.elapsed()<10000) {
                if(!process.waitForReadyRead(std::max<int>(1,10000-int(ready.elapsed())))) {break;}
            }
            if(!process.canReadLine()) {throw std::runtime_error("vdev did not print READY");}
            endpoint=QString::fromUtf8(process.readLine()).trimmed().mid(6);
            if(!endpoint.startsWith("tcp://") && !endpoint.startsWith("serial://")) {throw std::runtime_error("bad vdev endpoint");}
            if(!safe) {
                SessionOptions opts;opts.timeout=std::chrono::milliseconds(5000);
                Session s(endpoint.toStdString(),opts);Client c(s);auto info=c.info();
                std::vector<Ref> refs;std::vector<uint32_t> values;
                for(const auto &loop:info.loops) {refs.push_back({5,loop.id,1});values.push_back(10);}
                refs.push_back({0,0,defs::system::kWatchdogMs});values.push_back(1000);c.set(refs,values);
                // A reboot must retain the desktop scheduling allowance too.
                // Otherwise the reconnect case silently returns to a 1 kHz loop
                // with an 8 ms group-swap deadline and a 200 ms watchdog.
                c.system(5);
            }
        } catch(const std::exception &e) {
            qWarning("Fixture setup failed: %s",e.what());
            process.kill();process.waitForFinished(10000);
            throw;
        } catch(...) {
            process.kill();process.waitForFinished(10000);
            throw;
        }
    }
    ~Fixture() {process.kill();process.waitForFinished(10000);}
    void command(const QString &action) {
        const auto temp=directory.path()+"/next";QFile file(temp);
        if(!file.open(QIODevice::WriteOnly)) {throw std::runtime_error("cannot write control");}
        file.write((QString::number(++sequence)+" "+action+"\n").toUtf8());file.close();
        const auto path=directory.path()+"/control";
        // On Windows, vdev's 100 ms scenario poll opens `control` with
        // std::ifstream without FILE_SHARE_DELETE, so remove/rename can fail
        // transiently with a sharing violation.
        QElapsedTimer publish;publish.start();
        bool published=false;
        while(publish.elapsed()<1000) {
            if((!QFile::exists(path) || QFile::remove(path)) && QFile::rename(temp,path)) {published=true;break;}
            QTest::qWait(10);
        }
        if(!published) {
            qWarning("Fixture::command(%s): cannot publish control",qPrintable(action));
            throw std::runtime_error("cannot publish control");
        }
        const auto acknowledgement=(QString("SCENARIO %1 %2\n").arg(sequence).arg(action.section(' ',0,0))).toUtf8();
        // The 100 ms poll is not a scheduling guarantee. Wait for this command's
        // acknowledgement, emitted after its effect and stats-file close.
        const auto acknowledged=waitFor([&]{
            output+=process.readAllStandardOutput();output.replace("\r\n","\n");
            return output.contains(acknowledgement) || process.state()==QProcess::NotRunning;
        });
        if(!acknowledged || !output.contains(acknowledgement)) {
            const auto msg=(QString("vdev did not acknowledge %1: %2 %3")
                .arg(sequence).arg(QString::fromUtf8(output),QString::fromUtf8(process.readAllStandardError()))).toStdString();
            qWarning("Fixture::command(%s): %s",qPrintable(action),msg.c_str());
            throw std::runtime_error(msg);
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
// Regression guard for issue #130 (Windows MinGW + Qt 6.12): Windows catches
// FLS thread-exit access violations in Qt6Core.dll (destroy_current_thread_data)
// after WerFault pauses the process, so CTest exits 0 unless we observe the
// first-chance exception via a Vectored Exception Handler.
class QtCoreFaultGuard {
public:
#ifdef _WIN32
    QtCoreFaultGuard() {
        s_faults.store(0,std::memory_order_relaxed);
        HMODULE mod=::GetModuleHandleW(L"Qt6Core.dll");
        if(!mod) {mod=::GetModuleHandleW(L"Qt6Cored.dll");}
        if(mod) {
            const auto base=reinterpret_cast<uintptr_t>(mod);
            const auto *dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS *>(base+size_t(dos->e_lfanew));
            s_base.store(base,std::memory_order_relaxed);
            s_end.store(base+size_t(nt->OptionalHeader.SizeOfImage),std::memory_order_relaxed);
        } else {
            s_base.store(0,std::memory_order_relaxed);
            s_end.store(0,std::memory_order_relaxed);
        }
        m_handle=::AddVectoredExceptionHandler(1,&QtCoreFaultGuard::handler);
    }
    ~QtCoreFaultGuard() {
        if(m_handle) {::RemoveVectoredExceptionHandler(m_handle);}
    }
    int faults() const {return s_faults.load(std::memory_order_relaxed);}
private:
    static LONG CALLBACK handler(PEXCEPTION_POINTERS info) {
        if(info && info->ExceptionRecord && info->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION) {
            const auto addr=reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
            const auto base=s_base.load(std::memory_order_relaxed);
            const auto end=s_end.load(std::memory_order_relaxed);
            if(addr>=base && addr<end) {s_faults.fetch_add(1,std::memory_order_relaxed);}
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void *m_handle=nullptr;
    static inline std::atomic<uintptr_t> s_base{0};
    static inline std::atomic<uintptr_t> s_end{0};
    static inline std::atomic<int> s_faults{0};
#else
    int faults() const {return 0;}
#endif
};

} // namespace drivelink::gui::test
