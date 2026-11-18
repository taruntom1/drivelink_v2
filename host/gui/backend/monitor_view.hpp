#pragma once
// The Monitor page's state for QML: the signal browser, plot rates, the link
// budget, the stacked plots and their sample rings, pause, window, cursors
// and the snapshot CSV. The connection feeds it the device snapshot, the
// group list and samples, and configures the groups its plans ask for
// (connection_monitor.cpp). Plots and rates are per connection, never saved.
#include "monitor_model.hpp"
#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

namespace drivelink::gui {
class MonitorView final : public QObject, public PlotSource {
    Q_OBJECT
    // Sections {key, title, subtitle, loop, isLoop, rows[{key, name, unit,
    // input, checked, color}]} in browser order.
    Q_PROPERTY(QVariantList browser READ browser NOTIFY changed)
    // Latest value text per plotted signal key (with its unit).
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    // Running loops: {loop, name, divider, index, choices[{divider, text}], plotted}.
    Q_PROPERTY(QVariantList rates READ rates NOTIFY changed)
    // {load, percent, text, groups, total, lost, note}.
    Q_PROPERTY(QVariantMap link READ link NOTIFY changed)
    // {index, unit, series[{key, name, owner, unit, color, input}]}.
    Q_PROPERTY(QVariantList plots READ plots NOTIFY changed)
    // The last refused change: {key, loop, reason, suggestion, divider}; empty when none.
    Q_PROPERTY(QVariantMap refusal READ refusal NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(int windowSeconds READ windowSeconds NOTIFY changed)
    Q_PROPERTY(QVariantList windowChoices READ windowChoices CONSTANT)
    Q_PROPERTY(bool cursors READ cursors NOTIFY changed)
    Q_PROPERTY(double cursorA READ cursorA NOTIFY cursorsChanged)
    Q_PROPERTY(double cursorB READ cursorB NOTIFY cursorsChanged)
    // {dt, rows[{key, name, color, a, b, delta}]} while the cursors are shown.
    Q_PROPERTY(QVariantMap readout READ readout NOTIFY cursorsChanged)
    // Device time of the newest sample (frozen while paused), in µs.
    Q_PROPERTY(double nowUs READ nowUs NOTIFY dataChanged)
    Q_PROPERTY(int plotted READ plotted NOTIFY changed)
public:
    explicit MonitorView(QObject *parent=nullptr) : QObject(parent) {}
    QVariantList browser() const;
    QVariantMap values() const;
    QVariantList rates() const;
    QVariantMap link() const;
    QVariantList plots() const;
    QVariantMap refusal() const { return m_refusal; }
    bool paused() const { return m_paused; }
    int windowSeconds() const { return m_window; }
    QVariantList windowChoices() const { return {2,5,10,30,60}; }
    bool cursors() const { return m_cursors; }
    double cursorA() const { return double(m_cursorA); }
    double cursorB() const { return double(m_cursorB); }
    QVariantMap readout() const;
    double nowUs() const { return double(m_now); }
    int plotted() const { return int(m_set.count()); }

    // A checked signal is removed; an unchecked one is added when it fits,
    // else the refusal says why (nothing changes by itself).
    Q_INVOKABLE void toggleSignal(const QString &key);
    Q_INVOKABLE void removeSignal(const QString &key);
    // Layout's "Plot in Monitor": a node's signals (not its inputs), each
    // checked like a click; the first that does not fit stops with its
    // refusal. Returns how many were added.
    Q_INVOKABLE int plotNode(const QString &nodeKey);
    Q_INVOKABLE void setRate(int loop, int divider);
    // The refusal's lower rate, and its signal.
    Q_INVOKABLE void applySuggestion();
    Q_INVOKABLE void dismissRefusal();
    Q_INVOKABLE void addPlot();
    Q_INVOKABLE void removePlot(int index);
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void setWindowSeconds(int seconds);
    // Cursors are placed while paused only; Resume hides them.
    Q_INVOKABLE void setCursors(bool shown);
    Q_INVOKABLE void setCursor(int which, double us);
    // Writes the visible window to <directory>/<device id>-<time>.csv
    // (Documents/DriveLink/snapshots by default) and returns the path ("" on failure).
    Q_INVOKABLE QString snapshot(const QString &deviceId);

    // Connection side.
    void setSnapshot(std::shared_ptr<const DeviceSnapshot>);
    // From the last group list: every other configured subscription group,
    // free slots, the slots in use and the total.
    void setLink(LinkContext, size_t used, size_t total);
    void setNote(const QString &note);
    // Counted during a poll and published once by endAppend.
    void addLost(uint64_t frames);
    // Where a group's samples go, looked up once per poll: the ring (null
    // when the signal is not plotted) and the value type.
    struct Sink {
        SampleRing *ring=nullptr;
        ValueType type=ValueType::U32;
        QString key;
    };
    std::vector<Sink> sinks(std::span<const Ref>);
    // A sample into the ring (not while paused).
    void push(const Sink &sink, int64_t us, uint32_t bits) {
        if(m_paused || !sink.ring) {return;}
        sink.ring->push(us,sampleValue(sink.type,bits));
        if(us>m_now) {m_now=us;}
        m_appended=true;
    }
    // The newest value of a signal, for the browser.
    // Plotted signals only: a group still streaming a signal just removed
    // (its pass is in flight) cannot bring its value back.
    void setLatest(const Sink &sink, uint32_t bits) {
        if(!sink.ring) {return;}
        m_latest[sink.key]=bits;m_valuesDirty=true;
    }
    // After a poll: notifies the values and the plots once.
    void endAppend();
    // The connection closed: plots, rates and data go.
    void reset();
    // The link was lost (a requested reboot reopens it): data goes, the plot
    // set and rates stay for the same device.
    void lose();
    std::vector<MonitorPlan> plans() const;
    const SampleRing *ring(const QString &key) const override;
    int64_t windowStart() const { return m_now-int64_t(m_window)*1000000; }
    int64_t plotStart() const override { return windowStart(); }
    int64_t plotEnd() const override { return m_now; }
    void setSnapshotDirectory(const QString &directory) { m_directory=directory; }
    const PlotSet &plotSet() const { return m_set; }
signals:
    void changed();
    void valuesChanged();
    void dataChanged();
    void cursorsChanged();
    // The plans changed: the connection reconfigures its groups.
    void plansChanged();
    void notification(QString text, int tone);
private:
    std::shared_ptr<const DeviceSnapshot> m_snapshot;
    std::vector<MonitorSection> m_catalogue;
    PlotSet m_set;
    QHash<QString,int> m_colors;
    QHash<QString,SampleRing> m_rings;
    QHash<QString,uint32_t> m_latest;
    LinkContext m_link;
    size_t m_used=0,m_total=0;
    uint64_t m_lost=0;
    QString m_note,m_directory;
    QVariantMap m_refusal;
    bool m_paused=false,m_cursors=false,m_appended=false,m_valuesDirty=false,m_lostDirty=false;
    int m_window=10;
    int64_t m_now=0,m_cursorA=0,m_cursorB=0;
    void apply(const PlotSet &next);
    void refuse(const QString &key, uint8_t loop, const MonitorCheck &);
    void clearData();
    double rateOf(Ref) const;
    std::optional<MonitorSignal> signalOf(const QString &key) const;
};
} // namespace drivelink::gui
