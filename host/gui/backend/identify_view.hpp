#pragma once
// Plant identification for QML (H2e-3, G5 Tune · Plant identification),
// `connection.tune.identify`: the tab, the three cards (1 Check, 2 Run the
// step, 3 Model and suggested gains) and the measured vs fitted plot. The
// rules are in identify_model; the connection runs the step through the Tune
// run path (connection_tune.cpp) with the mode switched for it and written
// back afterwards; TuneView applies the suggested gains as one live batch.
//
// The step's controller is Tune's selected controller. A result is kept per
// controller in memory (not in session files) until the controller, the
// device or the connection changes.
#include "identify_model.hpp"
#include "tune_session.hpp"
#include <QObject>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace drivelink::gui {

class IdentifyView final : public QObject, public PlotSource {
    Q_OBJECT
    // The tab is shown (the right column shows the identification cards).
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY changed)
    // {available, reason}: whether the selected controller can be identified.
    Q_PROPERTY(QVariantMap availability READ availability NOTIFY changed)
    // Step inputs: {key, name, unit}.
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    // The mode during the step: index 0 is "unchanged", then {text} per enum value.
    Q_PROPERTY(QStringList modes READ modes NOTIFY changed)
    // {input, inputName, inputUnit, mode (index into modes), from, to, seconds,
    //  problem, summary ("duty_sp step 0 → 0.30 · captured 1.0 s"), capture
    //  ("2 signals at 1000 Hz · 23 KB of 4 MB")}.
    Q_PROPERTY(QVariantMap step READ step NOTIFY changed)
    // The operator's "nothing can be harmed if it moves" confirmation; cleared
    // by a DISARM, a controller change and a new connection.
    Q_PROPERTY(bool confirmed READ confirmed WRITE setConfirmed NOTIFY changed)
    // Card 1: {status (CheckStatus), label, detail, tag}.
    Q_PROPERTY(QVariantList checks READ checks NOTIFY changed)
    // Card 2: {active, stopping, text, progress (-1 unknown), canRun, blocked, done}.
    Q_PROPERTY(QVariantMap run READ run NOTIFY runChanged)
    // Card 3: {available, fitting, time, summary, end ("" or why the step was
    //  cut), responses [text], response (index), gain, gainUnit, timeConstant,
    //  deadTime, error, quality (0 good, 1 fair, 2 poor), qualityText, problem}.
    Q_PROPERTY(QVariantMap result READ result NOTIFY changed)
    // {gains [{key, name, current, suggested, clamped}], problem, canApply,
    //  applyReason, speed (index into speeds), closedLoop ("τc 12 ms")}.
    Q_PROPERTY(QVariantMap suggestion READ suggestion NOTIFY changed)
    Q_PROPERTY(QStringList speeds READ speeds CONSTANT)
    // The plot: {unit, title, series [{key, colour, target}], legend [{colour,
    // text}], markers [{us, colour, text}]} like Tune's plots; empty without a result.
    Q_PROPERTY(QVariantList plots READ plots NOTIFY changed)
    Q_PROPERTY(double plotStartUs READ plotStartUs NOTIFY changed)
    Q_PROPERTY(double plotEndUs READ plotEndUs NOTIFY changed)
public:
    // DLCheckItem's states, plus Warning (H2e-3).
    enum CheckStatus { Pass=0, Fail=1, Active=2, Pending=3, Warning=4 };
    Q_ENUM(CheckStatus)
    struct Hooks {
        std::function<QString()> runBlocked; // why no run may start ("" when one may)
        std::function<void()> start, stop;
        std::function<uint64_t()> armEpoch;  // this window's ARM, 0 while not armed by it
        // Applies a suggestion (one live batch, a history entry); "" or why not.
        std::function<QString(const ResolvedGains &, const QString &note)> apply;
        std::function<QString()> applyBlocked; // why Tune cannot write gains now
    };
    explicit IdentifyView(QObject *parent=nullptr);
    ~IdentifyView() override;
    void setHooks(Hooks hooks) { m_hooks=std::move(hooks); }

    bool shown() const { return m_shown; }
    void setShown(bool);
    QVariantMap availability() const;
    QVariantList inputs() const;
    QStringList modes() const;
    QVariantMap step() const;
    bool confirmed() const { return m_confirmed; }
    void setConfirmed(bool);
    QVariantList checks() const;
    QVariantMap run() const;
    QVariantMap result() const;
    QVariantMap suggestion() const;
    QStringList speeds() const;
    QVariantList plots() const;
    double plotStartUs() const { return double(plotStart()); }
    double plotEndUs() const { return double(plotEnd()); }
    const SampleRing *ring(const QString &key) const override;
    int64_t plotStart() const override;
    int64_t plotEnd() const override;

    Q_INVOKABLE void setInput(const QString &key);
    Q_INVOKABLE void setMode(int index);
    // from, to, seconds: "" or why not.
    Q_INVOKABLE QString setField(const QString &field, const QString &text);
    Q_INVOKABLE void startStep();
    Q_INVOKABLE void stopStep();
    Q_INVOKABLE void setResponse(int index);
    Q_INVOKABLE void setSpeed(int index);
    Q_INVOKABLE void applySuggested();

    // Tune's side.
    void setContext(std::shared_ptr<const DeviceSnapshot>, std::optional<InstanceKey> controller, const QString &deviceId);
    void reset();
    void refreshRunState();
    // The connection's side.
    struct Setup {
        RunSetup run;
        IdentifyStep step;
        std::optional<ModeChoice> mode;     // the value to hold during the step
        std::optional<uint32_t> previous;   // the enum's value before it (to write back)
        bool openLoop=false;                // the step runs open loop (gains may be suggested)
    };
    std::optional<Setup> setup(QString *problem=nullptr) const;
    enum class Phase { Idle, Switching, Starting, Running, Capturing, Reading, Restoring };
    void setPhase(Phase, const QString &text={}, double progress=-1);
    Phase phase() const { return m_phase; }
    bool active() const { return m_phase!=Phase::Idle; }
    void setStopping(bool on) { m_stopping=on; emit runChanged(); }
    // A finished step: fitted on a worker, then shown.
    void addResult(std::shared_ptr<const DeviceSnapshot>, RunRecord, const Setup &);
    // Tests: wait for a fit in progress.
    void waitFit();
    const IdentifyResult *current() const;
signals:
    void changed();
    void runChanged();
    void dataChanged();
    void notification(QString text, int tone);
private:
    Hooks m_hooks;
    std::shared_ptr<const DeviceSnapshot> m_snapshot;
    std::optional<InstanceKey> m_controller;
    QString m_deviceId;
    bool m_shown=false, m_confirmed=false, m_stopping=false;
    uint64_t m_lastEpoch=0;
    IdentifyStep m_step;
    int m_mode=-1; // index into modeChoices, -1 unchanged
    bool m_modeChosen=false; // the user picked it (else the default follows the controller)
    Phase m_phase=Phase::Idle;
    QString m_phaseText;
    double m_progress=-1;
    std::optional<IdentifyResult> m_result;
    std::optional<InstanceKey> m_resultController;
    QString m_resultTime;
    std::vector<SampleRing> m_rings; // measured, fitted
    int m_speed=int(TuneSpeed::Balanced);
    std::unique_ptr<QThread> m_fitThread;
    std::optional<IdentifyResult> m_fitResult;
    uint64_t m_fitSerial=0;
    QString m_fitTime;

    std::optional<TuneController> controllerInfo() const;
    const ItemDescriptor *inputDescriptor() const;
    double loopRate() const;
    uint32_t capacity() const;
    uint32_t maxRefs() const;
    void defaults();
    void rebuildRings();
    std::optional<Suggestion> currentSuggestion() const;
    void stopFit();
    void fitDone();
};
} // namespace drivelink::gui
