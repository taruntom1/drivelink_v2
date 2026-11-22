#pragma once
// Plant identification's rules (H2e-3, G5 Tune · Plant identification), pure:
// what an open-loop step needs on a controller, the step's test signal and
// capture, the first-order-plus-dead-time fit of each captured response
// (drivelink/tuning.hpp) and the suggested gains on the controller's params.
// Nothing here touches a device or Qt Quick.
//
// Conventions (user decision 2026-11-18, docs/adding-a-module.md), never
// module names:
// - A tune-flagged numeric group (the text before the first dot) with
//   members `<prefix>.kp` and `<prefix>.ki` (and optionally `<prefix>.kd`) is
//   a parallel PID u = kp·e + ki·∫e dt + kd·de/dt.
// - The responses are the instance's f32 signals with a unit whose name a PID
//   group's prefix starts (`velocity` -> `vel.*`, `position` -> `pos.*`);
//   suggestions go to that group.
// - During the step a tune-flagged enum is set to the value named `open_loop`
//   (the default; the card may pick another value or leave it unchanged) and
//   written back afterwards.
// - The step input is a commandable testgen input (the hold rule, so it
//   returns to 0 afterwards), by default the first without a unit.
#include "tune_model.hpp"
#include <drivelink/tuning.hpp>
#include <optional>
#include <vector>

namespace drivelink::gui {

struct PidGroup {
    QString prefix;
    uint16_t kp=0, ki=0;
    std::optional<uint16_t> kd;
};
// The instance's PID groups, by item id of `kp`.
std::vector<PidGroup> pidGroups(const DeviceSnapshot &, InstanceKey);
// The group whose prefix starts `signalName` (the longest such prefix).
std::optional<PidGroup> groupFor(const std::vector<PidGroup> &, const QString &signalName);

// A value of a tune-flagged enum to hold during the step.
struct ModeChoice {
    uint16_t item=0;
    uint32_t value=0;
    QString name;   // the value's name
    QString label;  // "mode: open_loop"
};
std::vector<ModeChoice> modeChoices(const DeviceSnapshot &, InstanceKey);
// The value named `open_loop`, if one of the instance's tune enums has it.
std::optional<ModeChoice> defaultMode(const DeviceSnapshot &, InstanceKey);
// The device's current value of the choice's enum.
std::optional<uint32_t> enumValue(const DeviceSnapshot &, InstanceKey, uint16_t item);

// The step inputs: the controller's commandable testgen targets.
std::vector<TuneTarget> stepInputs(const TuneController &);
std::optional<TuneTarget> defaultStepInput(const TuneController &);
// The instance's response candidates (see above), in item order.
std::vector<Ref> responseCandidates(const DeviceSnapshot &, InstanceKey);

// The step: `from` held for `seconds` (the baseline, also when it is 0),
// then `to` for `seconds`: one Square period (offset (from+to)/2, amplitude
// (from−to)/2), so one START gives both halves.
struct IdentifyStep {
    Ref input{};
    double from=0, to=0.3, seconds=1;
};
TestSignal identifySignal(const IdentifyStep &);
// Empty when the step can run: the signal checks of a Tune run, a step that
// moves, and at least 20 loop periods in each half.
QString identifyProblem(const IdentifyStep &, double loopRateHz, const ItemDescriptor &input);
// The input and the candidates (at most `maxRefs`), over the whole signal at
// the loop rate (a divider when the capacity needs it), ≈6 % before it.
CaptureSetup identifyCapture(const IdentifyStep &, const std::vector<Ref> &candidates, double loopRateHz,
                             uint32_t capacity, uint32_t maxRefs);

// One response's model.
struct ResponseFit {
    int index=-1;   // into the run's refs
    QString name, unit;
    std::optional<PidGroup> group;
    FopdtFit fit;
};
// A finished identification: the capture (as a run record) and a fit per
// captured candidate; `response` is the one shown.
struct IdentifyResult {
    RunRecord run;
    IdentifyStep step;
    std::optional<ModeChoice> mode;
    QString controllerLabel;
    double loopPeriodS=0;
    // The step ran open loop: the `open_loop` value held (or already set),
    // or no tune enum and a unitless input. Only then are gains suggested.
    bool openLoop=false;
    int64_t stepEndUs=0; // the last sample of the step (the fit's window)
    std::vector<ResponseFit> fits;
    int response=-1; // index into fits
};
// Fits every candidate over the samples up to the signal's end (or the Stop
// or disarm); the shown response is the settled one with the smallest error.
IdentifyResult makeIdentify(const DeviceSnapshot &, RunRecord, const IdentifyStep &, std::optional<ModeChoice>,
                            double loopRateHz);

// The suggested gains for a fit on the controller's group: kp and ki from
// SIMC, kd 0 when the group has one; each rounded to three digits and clamped
// to its range (marked).
struct SuggestedGain {
    uint16_t item=0;
    QString name;
    double current=0, suggested=0;
    bool clamped=false;
};
struct Suggestion {
    std::vector<SuggestedGain> gains;
    PiGains pi;
    QString problem; // empty when the gains may be applied
};
Suggestion suggestGains(const DeviceSnapshot &, InstanceKey, const ResponseFit &, TuneSpeed, double loopPeriodS,
                        bool openLoop);
// Whether a step with this mode choice (none: unchanged) runs open loop.
bool runsOpenLoop(const DeviceSnapshot &, InstanceKey, const std::optional<ModeChoice> &, const ItemDescriptor &input);

QString qualityText(FitQuality);
// "rad/s per unit" (the input has no unit) or "rad/s per V".
QString gainUnit(const QString &responseUnit, const QString &inputUnit);
} // namespace drivelink::gui
