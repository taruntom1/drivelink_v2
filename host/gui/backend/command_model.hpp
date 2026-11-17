#pragma once
// Monitor's manual command rules (H2d-2, G5 CommandControl), pure: which
// inputs may be commanded (the hold rule), each input's slider span, limit
// and step, the dead-man ramp, Latch, and the value frames due per command
// group. Nothing here touches a device or Qt Quick; the caller supplies the
// time and writes the frames.
//
// The hold rule (user decision 2026-11-17): only numeric inputs whose
// descriptor range is finite and includes 0 are commanded; they sit in value
// groups whose hold is 0. Every value a frame carries is one the card shows
// (quantized to the shown step) or 0.
#include <drivelink/model.hpp>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace drivelink::gui {

// Empty when the input can be commanded; else why not, for the card.
QString commandRefusal(const ItemDescriptor &);
inline bool commandable(const ItemDescriptor &d) { return commandRefusal(d).isEmpty(); }
double bitsToNumber(ValueType, uint32_t bits);
// `value` must already lie in the type's range (integral for integer types).
uint32_t numberToBits(ValueType, double value);

struct CommandInputSpec { Ref ref; ItemDescriptor descriptor; };
// One value group as configured: its inputs in configuration order.
struct CommandGroupSpec { uint8_t id=0; std::vector<CommandInputSpec> inputs; };

class CommandModel {
public:
    // Release ramps linearly to 0 in 12 steps of 20 ms; frames of one group
    // are at least 20 ms apart (latest values win).
    static constexpr int64_t kStepMs=20;
    static constexpr int kRampSteps=12;

    struct Input {
        Ref ref;
        ItemDescriptor descriptor;
        size_t group=0;       // index into the configured groups
        double lower=0,upper=0; // descriptor range
        double bound=0;       // the largest limit: max(|lower|, |upper|)
        double limit=0;
        double step=0.01;     // the shown resolution; values are multiples of it
        int decimals=2;
        double value=0;
        bool shown=false;     // on the card
        bool latched=false,held=false,ramping=false;
        int writes=0;         // live param writes in flight to its instance
        double rampFrom=0;
        int64_t rampStart=0;
        // The slider span: [-limit, +limit] within the descriptor range.
        double from() const { return std::max(lower,-limit); }
        double to() const { return std::min(upper,limit); }
    };
    struct Frame { uint8_t group=0; std::vector<uint32_t> bits; };

    // Replaces every group and input (values 0, nothing latched, nothing
    // shown). Inputs that fail the hold rule are refused (Usage error).
    void configure(const std::vector<CommandGroupSpec> &);
    void clear();
    const std::vector<Input> &inputs() const { return m_inputs; }
    int find(const Ref &) const;
    // Commands are accepted only while enabled. Disabling returns every
    // value to 0 locally, unlatches and drops unsent frames: the firmware
    // resets its inputs at DISARM and ARM, so nothing is sent.
    void setEnabled(bool);
    bool enabled() const { return m_enabled; }

    static double defaultLimit(const Input &);
    // Clamped to (0, bound]; the step follows the limit and a value outside
    // the new span is brought inside it.
    void setLimit(int index, double limit);
    void setShown(int index, bool shown);

    // The slider: press, drag while pressed, release. A release without
    // Latch ramps to 0; with Latch it keeps the value.
    bool press(int index);
    bool drag(int index, double value);
    void release(int index, int64_t nowMs);
    // Every pressed slider loses its press (focus loss, page change).
    void releaseHeld(int64_t nowMs);
    // Unlatching a non-zero value that is not pressed ramps it to 0.
    void setLatched(int index, bool on, int64_t nowMs);
    // A typed value, accepted only while latched. Returns the value applied.
    std::optional<double> type(int index, double value);
    // Remove from the card: the input returns to 0 at once.
    void remove(int index);
    // Every input to 0 at once, unlatched.
    void holdAll();
    // A live param write to this instance (user decisions 2026-11-18): when
    // it starts, its inputs go to 0 at once, unlatched, and take no press,
    // Latch or typed value until it ends; every frame meanwhile carries 0
    // for them. When it ends (reply, refusal or uncertain outcome), their
    // group sends a fresh frame even when the values were 0 already.
    void beginWrite(uint8_t type, uint8_t instance);
    void endWrite(uint8_t type, uint8_t instance);
    void zeroInstance(uint8_t type, uint8_t instance);

    // Advances ramps and returns the frames due now. A frame the caller
    // could not queue goes back with retry().
    std::vector<Frame> tick(int64_t nowMs);
    void retry(uint8_t group);
    // Ramps running or frames waiting: the caller keeps ticking.
    bool active() const;
    // The value nearest `value` in the input's span, on its step.
    double clampValue(const Input &, double value) const;

private:
    struct Group { uint8_t id=0; std::vector<size_t> inputs; bool dirty=false; int64_t lastSent=0; bool sentOnce=false; };
    std::vector<Group> m_groups;
    std::vector<Input> m_inputs;
    bool m_enabled=false;
    void setValue(Input &, double value);
    void resetValues();
    static void applyLimit(Input &, double limit);
};
} // namespace drivelink::gui
