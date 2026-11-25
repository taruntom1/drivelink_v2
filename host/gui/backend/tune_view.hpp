#pragma once
// The Tune page's state for QML (H2e-1, H2e-2, G5 Tune), `connection.tune`:
// the controller picker, the gains card (in-place rows: a drag survives
// updates) with its sender, the test signal and capture cards, the run in
// progress, the runs with their overlay and metrics, the gain history with
// Restore and A/B, Auto re-run and the session (files, autosave). The rules
// are in tune_model and tune_session; the connection writes params, runs the
// test signal and capture (connection_tune.cpp) and reports back here. Runs
// and history are kept per device in memory and in a recovery file.
#include "identify_view.hpp"
#include "tune_model.hpp"
#include "tune_session.hpp"
#include <QAbstractListModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace drivelink::gui {

class TuneView;
class GainRows final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { Key=Qt::UserRole+1, Name, Description, Unit, Value, Text, Min, Max, WindowLo, WindowHi, Position,
                Saved, HasSaved, Ticks, Zero, Negative, Dragging, Sending, Problem, Group, GroupTitle, GroupFirst,
                GroupOpen, GroupNote, Header };
    explicit GainRows(TuneView &view) : m_view(view) {}
    int rowCount(const QModelIndex &p={}) const override;
    QVariant data(const QModelIndex &, int) const override;
    QHash<int,QByteArray> roleNames() const override;
private:
    friend class TuneView;
    TuneView &m_view;
};

class RunRows final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { Number=Qt::UserRole+1, Colour, Time, Changed, Legend, End, Pinned, Shown, Faded, Rise, Overshoot,
                Settling, SteadyError, MetricsNote, Response, Irregular };
    explicit RunRows(TuneView &view) : m_view(view) {}
    int rowCount(const QModelIndex &p={}) const override;
    QVariant data(const QModelIndex &, int) const override;
    QHash<int,QByteArray> roleNames() const override;
private:
    friend class TuneView;
    TuneView &m_view;
};

// The gain history of the selected controller, newest first.
class HistoryRows final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { Id=Qt::UserRole+1, Time, Change, Note, Run, RunColour, Current, Mark, CanRestore, RestoreReason,
                CanCompare, CompareReason, Foreign };
    explicit HistoryRows(TuneView &view) : m_view(view) {}
    int rowCount(const QModelIndex &p={}) const override;
    QVariant data(const QModelIndex &, int) const override;
    QHash<int,QByteArray> roleNames() const override;
private:
    friend class TuneView;
    TuneView &m_view;
    std::vector<size_t> m_rows; // indices into the history
};

class TuneView final : public QObject, public PlotSource {
    Q_OBJECT
    // {key, text, subtitle} per tunable instance.
    Q_PROPERTY(QVariantList controllers READ controllers NOTIFY changed)
    Q_PROPERTY(QString controller READ controller NOTIFY changed)
    // Tune-flagged enums: {key, name, options[text], index, sending, problem}.
    Q_PROPERTY(QVariantList enums READ enums NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel* gains READ gains CONSTANT)
    Q_PROPERTY(int gainCount READ gainCount NOTIFY changed)
    // {enabled, reason} for "Revert gains to saved".
    Q_PROPERTY(QVariantMap revert READ revert NOTIFY changed)
    // {key, name, unit} per test signal target of the controller.
    Q_PROPERTY(QVariantList targets READ targets NOTIFY changed)
    // {target, waveform, offset, amplitude, f0, f1, duration, repeat, unit,
    //  usesFrequency, chirp, problem, holds}.
    Q_PROPERTY(QVariantMap testSignal READ testSignal NOTIFY changed)
    Q_PROPERTY(QStringList waveforms READ waveforms CONSTANT)
    // {refs[{key, name, unit}], candidates[{key, name, unit, enabled, reason}],
    //  rates[{divider, text}], rateIndex, samples, pre, usage, problem}.
    Q_PROPERTY(QVariantMap capture READ capture NOTIFY changed)
    // {active, stopping, text, progress, canRun, blocked}.
    Q_PROPERTY(QVariantMap run READ run NOTIFY runChanged)
    Q_PROPERTY(QAbstractItemModel* runs READ runs CONSTANT)
    Q_PROPERTY(int runCount READ runCount NOTIFY changed)
    // The captured signals of the shown runs: {key, name, unit, checked}.
    Q_PROPERTY(QVariantList chips READ chips NOTIFY changed)
    // One plot per unit of the checked chips: {unit, title, series[{key,
    // colour, faded, target}], legend[{colour, text}], markers[{us, colour, text}]}.
    Q_PROPERTY(QVariantList plots READ plots NOTIFY changed)
    Q_PROPERTY(double plotStartUs READ plotStartUs NOTIFY changed)
    Q_PROPERTY(double plotEndUs READ plotEndUs NOTIFY changed)
    // H2e-2. The selected controller's gain history (HistoryRows).
    Q_PROPERTY(QAbstractItemModel* history READ history CONSTANT)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY changed)
    // {active, picking, a{id, run, colour, time}, b{...}, gainChanges, equal,
    //  metrics[{name, a, b, change}]}.
    Q_PROPERTY(QVariantMap compare READ compare NOTIFY changed)
    // {on, allowed, pending, text}: Auto re-run.
    Q_PROPERTY(QVariantMap autoRerun READ autoRerun NOTIFY runChanged)
    // {name, file, unsaved, canSave, saveReason, canOpen, openReason, folder}.
    Q_PROPERTY(QVariantMap session READ session NOTIFY changed)
    // {available, time}: an autosaved session of this device to restore.
    Q_PROPERTY(QVariantMap recovery READ recovery NOTIFY changed)
    // H2e-3. Plant identification (IdentifyView).
    Q_PROPERTY(QObject* identify READ identify CONSTANT)
    Q_PROPERTY(bool identifying READ identifying NOTIFY changed)
public:
    // Writes live params (one batch, one loop); `done` gets "" or the refusal.
    using Writer=std::function<void(std::vector<Ref>, std::vector<uint32_t>, std::function<void(QString)>)>;
    struct Hooks {
        Writer write;
        std::function<QString()> runBlocked; // why a run cannot start ("" when it can)
        std::function<void()> start, stop;
        // H2e-2. Ends the run in progress without keeping it (a gain changed
        // during a run while Auto re-run is on).
        std::function<void()> cancel;
        // This window's accepted ARM: a number that changes with every ARM,
        // 0 while not armed by this window.
        std::function<uint64_t()> armEpoch;
        // Why an automatic run may not start now ("" when it may), beyond Run's rules.
        std::function<QString()> autoBlocked;
    };
    explicit TuneView(QObject *parent=nullptr);
    ~TuneView() override;
    void setHooks(Hooks hooks) { m_hooks=std::move(hooks); }

    QVariantList controllers() const;
    QString controller() const;
    QVariantList enums() const;
    QAbstractItemModel *gains() { return &m_gainRows; }
    int gainCount() const { return int(m_gains.size()); }
    // A gain or window bound as the gains card shows it.
    Q_INVOKABLE QString formatGain(double value) const { return drivelink::gui::gainText(value); }
    QVariantMap revert() const;
    QVariantList targets() const;
    QVariantMap testSignal() const;
    QStringList waveforms() const;
    QVariantMap capture() const;
    QVariantMap run() const;
    QAbstractItemModel *runs() { return &m_runRows; }
    int runCount() const { return int(m_runs.size()); }
    QVariantList chips() const;
    QVariantList plots() const;
    double plotStartUs() const { return double(plotStart()); }
    double plotEndUs() const { return double(plotEnd()); }
    const SampleRing *ring(const QString &key) const override;
    int64_t plotStart() const override;
    int64_t plotEnd() const override;

    Q_INVOKABLE void selectController(const QString &key);
    Q_INVOKABLE void setEnum(const QString &key, int index);
    // The log slider: press, drag to a position (0..1 of the window), release.
    Q_INVOKABLE void gainPress(const QString &key);
    Q_INVOKABLE void gainDrag(const QString &key, double position);
    Q_INVOKABLE void gainRelease(const QString &key);
    // A typed value (Enter): "" or why not.
    Q_INVOKABLE QString gainText(const QString &key, const QString &text);
    // ÷2 0.5, −10 % 0.9, +10 % 1.1, ×2 2; keys: 1 %, Shift 10 %, Ctrl 0.1 %.
    Q_INVOKABLE void gainStep(const QString &key, double factor);
    Q_INVOKABLE void gainKey(const QString &key, bool up, bool shift, bool ctrl);
    // A typed slider window: "" or why not.
    Q_INVOKABLE QString setWindow(const QString &key, const QString &lo, const QString &hi);
    Q_INVOKABLE void revertGains();
    Q_INVOKABLE void setTarget(const QString &key);
    Q_INVOKABLE void setWaveform(int index);
    // offset, amplitude, f0, f1, duration (s), repeat: "" or why not.
    Q_INVOKABLE QString setSignalField(const QString &field, const QString &text);
    Q_INVOKABLE void addCaptureRef(const QString &key);
    Q_INVOKABLE void removeCaptureRef(const QString &key);
    Q_INVOKABLE void setDivider(int divider);
    Q_INVOKABLE QString setSamples(const QString &samples, const QString &pre);
    Q_INVOKABLE void toggleChip(const QString &key);
    Q_INVOKABLE void startRun();
    Q_INVOKABLE void stopRun();
    Q_INVOKABLE void pinRun(int number, bool pinned);
    Q_INVOKABLE void removeRun(int number);

    // H2e-2.
    QAbstractItemModel *history() { return &m_historyRows; }
    int historyCount() const { return int(m_historyRows.m_rows.size()); }
    QVariantMap compare() const;
    QVariantMap autoRerun() const;
    QVariantMap session() const;
    QVariantMap recovery() const;
    // Writes an entry's gains again (live).
    Q_INVOKABLE void restoreEntry(int id);
    // The A/B button of an entry: the first pick is A, the second B; picking
    // a marked entry unmarks it; a further pick replaces B.
    Q_INVOKABLE void pickCompare(int id);
    Q_INVOKABLE void exitCompare();
    Q_INVOKABLE void restoreCompare(bool b);
    Q_INVOKABLE void setAutoRerun(bool on);
    // Files: a URL or local path; "" or why not.
    Q_INVOKABLE QString saveSession(const QString &url);
    Q_INVOKABLE QString openSession(const QString &url);
    Q_INVOKABLE QString exportRun(int number, const QString &url);
    // A failure from QML (a dialog's result) as a toast.
    Q_INVOKABLE void report(const QString &text) { if(!text.isEmpty()) {emit notification(text,2);} }
    // The file name a save dialog offers (no folder).
    Q_INVOKABLE QString suggestedName(const QString &kind, int number=0) const;
    Q_INVOKABLE void restoreRecovery();
    Q_INVOKABLE void discardRecovery();
    // The newest run's number (0 without runs).
    Q_INVOKABLE int newestRun() const { return m_runs.empty()?0:m_runs.back().number; }

    // Connection side.
    void setSnapshot(std::shared_ptr<const DeviceSnapshot>, const QString &deviceId);
    // The connection closed or another device: the runs stay only for the same device.
    void reset();
    // Re-evaluate whether Run is possible (arm state, groups, another run);
    // an Auto re-run waiting for it is checked again.
    void refreshRunState();
    // Where autosaved sessions are kept (a folder; "" for none).
    void setRecoveryFolder(const QString &folder) { m_recoveryFolder=folder; }
    // Writes the autosave now when one is due (closing, another device).
    void flushAutosave();
    const std::vector<HistoryEntry> &historyList() const { return m_history; }
    bool unsaved() const { return m_dirty; }
    // What the next run uses; empty optional when the setup is not valid.
    using RunSetup=drivelink::gui::RunSetup;
    std::optional<RunSetup> runSetup(QString *problem=nullptr) const;
    // The run's progress, from the connection.
    enum class Phase { Idle, Starting, Running, Capturing, Reading };
    void setPhase(Phase, const QString &text={}, double progress=-1);
    Phase phase() const { return m_phase; }
    void setStopping(bool on) { m_stopping=on; emit runChanged(); }
    // A finished run joins the list (numbered, coloured, metrics computed).
    void addRun(RunRecord, const RunSetup &);
    const std::vector<RunRecord> &runList() const { return m_runs; }
    // H2e-3. Plant identification: its view, and while it runs (the step,
    // then the mode written back) the gains, mode, Restore, Revert, Run and
    // Auto re-run wait.
    QObject *identify() { return &m_identify; }
    IdentifyView &identifyView() { return m_identify; }
    void setIdentifying(bool on);
    bool identifying() const { return m_identifying; }
    // A gain or a tune enum (the mode) being written or dragged.
    bool writesPending() const { return gainsMoving() || !m_enumSending.isEmpty(); }
    // Why gains cannot be written now ("" when they can); applies a
    // suggestion as one live batch with a history entry ("" or why not).
    QString applyBlocked() const;
    QString applyGains(const ResolvedGains &, const QString &note);
    // Tests: the gain sender's state.
    bool gainBusy() const;
    // A gain write pending or in flight, or a gain being dragged.
    bool gainsMoving() const;
    void flushNow() { flush(true); }
signals:
    void changed();
    void runChanged();
    void dataChanged();
    void notification(QString text, int tone);
private:
    friend class GainRows;
    friend class RunRows;
    friend class HistoryRows;
    struct Gain {
        uint16_t item=0;
        QString key;
        double shown=0;
        std::optional<double> pending; // the latest value not yet sent
        // Written and accepted, not yet read back: shown until the device
        // value matches it or 2 s pass (the read-back follows a drag's pause).
        std::optional<double> applied;
        qint64 appliedAt=0;
        bool inflight=false, dragging=false;
        qint64 lastSent=-1000;
        std::optional<GainWindow> window, typed;
        QString problem;
    };
    // A group header (gain -1) or a gain of the group.
    struct Row { int gain=-1; QString group; bool groupFirst=false, groupOpen=true; QString groupNote; };
    std::shared_ptr<const DeviceSnapshot> m_snapshot;
    // What the page shows from a snapshot: unchanged by a status frame alone.
    std::vector<uint64_t> m_print;
    bool m_moving=false; // gainsMoving() as QML last saw it
    QString m_deviceId;
    std::optional<InstanceKey> m_controller;
    std::vector<Gain> m_gains;
    std::vector<Row> m_rows;
    GainRows m_gainRows;
    QHash<QString,QString> m_enumProblems;
    QHash<QString,bool> m_enumSending;
    // Tune-flagged values while the device reported no unsaved changes.
    std::map<std::tuple<uint8_t,uint8_t,uint16_t>,double> m_saved;
    bool m_savedKnown=false;
    TestSignal m_signal;
    CaptureSetup m_capture;
    bool m_captureEdited=false;
    QString m_captureNote;
    std::vector<RunRecord> m_runs;
    RunRows m_runRows;
    QHash<QString,bool> m_chips;
    int m_nextNumber=1, m_nextColour=0;
    QHash<QString,SampleRing> m_noRings;
    Phase m_phase=Phase::Idle;
    QString m_phaseText;
    double m_progress=-1;
    bool m_stopping=false;
    Hooks m_hooks;
    QTimer m_sendTimer;
    QElapsedTimer m_clock;
    // H2e-2. Typed slider windows per gain item key (kept across controller changes).
    QHash<QString,SessionWindow> m_windows;
    std::vector<HistoryEntry> m_history;
    HistoryRows m_historyRows;
    int m_nextHistory=1;
    std::optional<int> m_compareA, m_compareB; // history ids
    // Gains quiet for 0.5 s: a history entry, then an Auto re-run when due.
    QTimer m_settle, m_autoCheck, m_autosave;
    QString m_settleNote;
    HistorySource m_settleSource=HistorySource::Changed;
    // A gain changed and has not settled; an Auto re-run is due and waits for Run.
    bool m_autoOn=true, m_changePending=false, m_autoPending=false;
    uint64_t m_autoEpoch=0; // the ARM of the last manual Run
    QString m_autoNote;
    QString m_sessionName, m_sessionPath, m_recoveryFolder;
    bool m_dirty=false, m_autosaveDue=false, m_tooLargeNoted=false;
    // The autosave writes on a QThread worker from a copy (a large session
    // must not stall the GUI thread, where DISARM is handled, and QThread
    // tears down QThreadData cleanly on Windows MinGW).
    std::unique_ptr<QThread> m_autosaveThread;
    std::shared_ptr<QString> m_autosaveResult;
    std::optional<QDateTime> m_recoveryOffer;
    // H2e-3.
    IdentifyView m_identify;
    bool m_identifying=false;
    bool busy() const { return m_phase!=Phase::Idle || m_identifying; }

    std::optional<TuneController> currentController() const;
    const ModelItem *itemOf(uint16_t id) const;
    double deviceValue(uint16_t id) const;
    double rangeOf(uint16_t id, bool max) const;
    void rebuildGains();
    void updateGains();
    int gainIndex(const QString &key) const;
    void setGain(int index, double value, bool now);
    void flush(bool force);
    void defaultsForTarget();
    void reconcileCapture();
    double loopRate() const;
    void runsChanged();
    // H2e-2.
    bool gainsLocked() const;
    void gainChanged();
    void settled();
    void tryAuto();
    bool gainsDifferFromLastRun() const;
    bool recordCurrent(HistorySource, const QString &note);
    void refreshHistoryRows();
    void historyChanged();
    std::vector<OverlayEntry> shownRuns() const;
    const HistoryEntry *entryById(int id) const;
    QString restoreBlocked(const HistoryEntry &) const;
    void writeSet(const ResolvedGains &, HistorySource, const QString &note);
    void markDirty();
    QString recoveryPath() const;
    // `now`: write on this thread (closing, another device); else on a worker.
    void writeAutosave(bool now);
    void autosaveDone();
    void waitAutosave();
    QString tooLarge() const;
    SessionData sessionData() const;
    void applySession(SessionData, bool recovered);
    void clearSession();
};
} // namespace drivelink::gui
