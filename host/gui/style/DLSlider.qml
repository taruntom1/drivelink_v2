import QtQuick
import QtQuick.Templates as T

// Figma Slider. Scale=Symmetric: a linear track from `from` to `to` with a
// mark at 0, the span from 0 to the value filled, the handle, a value bubble
// while dragging and the range labels under the track. Values snap to
// `stepSize` and are shown with `decimals`.
// Scale=Log (Tune's gains, H2e): the value is the position 0..1 on a log
// window the owner maps; the owner supplies the three labels, the bubble text
// and `ticks` [{position, color}] (the saved value, the runs' gains), each at
// its own position on the log scale. The control only draws and emits; its
// owner sends.
T.Slider {
    id: control

    enum Scale { Symmetric, Log }
    property int scaleType: DLSlider.Symmetric
    readonly property bool log: scaleType === DLSlider.Log
    property string lowText
    property string midText
    property string highText
    property string bubbleText
    property var ticks: []

    property int decimals: 2
    // What the value bubble shows while dragging; an owner that quantizes
    // the value itself binds the value it sends.
    property real shownValue: value

    function format(v) {
        const text = Math.abs(v).toFixed(decimals)
        return Number(text) === 0 ? Number(0).toFixed(decimals) : (v > 0 ? "+" : "−") + text
    }
    readonly property real zeroPosition: to > from ? Math.max(0, Math.min(1, (0 - from) / (to - from))) : 0

    implicitWidth: 320
    implicitHeight: 52
    padding: 0
    leftPadding: 8
    rightPadding: 8
    snapMode: T.Slider.SnapAlways
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    readonly property real trackY: 22

    background: Item {
        x: control.leftPadding
        width: control.availableWidth
        height: control.height

        // The 0 mark above the track.
        Rectangle {
            visible: !control.log
            x: control.zeroPosition * parent.width - width / 2
            y: control.trackY - 12
            width: 1; height: 8
            color: Theme.textSecondary
        }
        Rectangle {
            id: track
            y: control.trackY - height / 2
            width: parent.width; height: 4
            radius: 2
            color: Theme.bgSecondary
            Rectangle {
                readonly property real origin: control.log ? 0 : control.zeroPosition
                readonly property real a: Math.min(origin, control.visualPosition) * track.width
                readonly property real b: Math.max(origin, control.visualPosition) * track.width
                x: a; width: b - a; height: parent.height
                radius: 2
                color: Theme.accentDefault
            }
        }
        // Log: tick marks above the track, each at its value's position.
        Repeater {
            model: control.log ? control.ticks : []
            delegate: Rectangle {
                required property var modelData
                x: modelData.position * parent.width - width / 2
                y: control.trackY - 12
                width: 2; height: 8
                color: modelData.color
            }
        }
        Text {
            y: control.trackY + 12
            text: control.log ? control.lowText : control.format(control.from)
            font: Theme.fontMonoSmall; color: Theme.textSecondary
        }
        Text {
            visible: control.log ? control.midText.length > 0
                                 : control.zeroPosition > 0.15 && control.zeroPosition < 0.85
            x: (control.log ? 0.5 : control.zeroPosition) * parent.width - width / 2
            y: control.trackY + 12
            text: control.log ? control.midText : "0"
            font: Theme.fontMonoSmall; color: Theme.textSecondary
        }
        Text {
            x: parent.width - width
            y: control.trackY + 12
            text: control.log ? control.highText : control.format(control.to)
            font: Theme.fontMonoSmall; color: Theme.textSecondary
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * control.availableWidth - width / 2
        y: control.trackY - height / 2
        width: 16; height: 16; radius: 8
        color: Theme.textPrimary
        border.width: 2
        border.color: control.visualFocus ? Theme.borderFocus : Theme.accentDefault

        Rectangle {
            visible: control.pressed
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.top
            anchors.bottomMargin: 6
            width: bubble.implicitWidth + 2 * Theme.space8
            height: bubble.implicitHeight + Theme.space4
            radius: Theme.radiusSm
            color: Theme.bgRaised
            border.width: Theme.borderWidth
            border.color: Theme.borderStrong
            Text {
                id: bubble
                anchors.centerIn: parent
                text: control.log ? control.bubbleText : control.format(control.shownValue)
                font: Theme.fontMonoSmall; color: Theme.textPrimary
            }
        }
    }
}
