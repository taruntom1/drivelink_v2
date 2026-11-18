#pragma once
// The Tune page's state for QML (H2e-1, G5 Tune), `connection.tune`: the
// controller picker, the gains card (in-place rows: a drag survives updates)
// with its sender, the test signal and capture cards, the run in progress and
// the runs with their overlay and metrics. The rules are in tune_model; the
// connection writes params, runs the test signal and capture
// (connection_tune.cpp) and reports back here. Runs are kept per device in
// memory (session files arrive in H2e-2).
#include "tune_model.hpp"
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
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
public:
    // Writes live params (one batch, one loop); `done` gets "" or the refusal.
    using Writer=std::function<void(std::vector<Ref>, std::vector<uint32_t>, std::function<void(QString)>)>;
    struct Hooks {
        Writer write;
        std::function<QString()> runBlocked; // why a run cannot start ("" when it can)
        std::function<void()> start, stop;
    };
    explicit TuneView(QObject *parent=nullptr);
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

    // Connection side.
    void setSnapshot(std::shared_ptr<const DeviceSnapshot>, const QString &deviceId);
    // The connection closed or another device: the runs stay only for the same device.
    void reset();
    // Re-evaluate whether Run is possible (arm state, groups, another run).
    void refreshRunState() { emit runChanged(); }
    // What the next run uses; empty optional when the setup is not valid.
    struct RunSetup {
        TuneController controller;
        TestSignal signal;
        CaptureSetup capture;
        std::map<uint16_t,double> gains;
    };
    std::optional<RunSetup> runSetup(QString *problem=nullptr) const;
    // The run's progress, from the connection.
    enum class Phase { Idle, Starting, Running, Capturing, Reading };
    void setPhase(Phase, const QString &text={}, double progress=-1);
    Phase phase() const { return m_phase; }
    void setStopping(bool on) { m_stopping=on; emit runChanged(); }
    // A finished run joins the list (numbered, coloured, metrics computed).
    void addRun(RunRecord, const RunSetup &);
    const std::vector<RunRecord> &runList() const { return m_runs; }
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
};
} // namespace drivelink::gui
