import QtQuick
import QtQuick.Templates as T

// Figma Slider, Scale=Symmetric: a linear track from `from` to `to` with a
// mark at 0, the span from 0 to the value filled, the handle, a value bubble
// while dragging and the range labels under the track. Values snap to
// `stepSize` and are shown with `decimals`. (The Log scale of Tune's gains
// arrives with H2e.) The control only draws and emits; its owner sends.
T.Slider {
    id: control

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
                readonly property real a: Math.min(control.zeroPosition, control.visualPosition) * track.width
                readonly property real b: Math.max(control.zeroPosition, control.visualPosition) * track.width
                x: a; width: b - a; height: parent.height
                radius: 2
                color: Theme.accentDefault
            }
        }
        Text {
            y: control.trackY + 12
            text: control.format(control.from)
            font: Theme.fontMonoSmall; color: Theme.textSecondary
        }
        Text {
            visible: control.zeroPosition > 0.15 && control.zeroPosition < 0.85
            x: control.zeroPosition * parent.width - width / 2
            y: control.trackY + 12
            text: "0"
            font: Theme.fontMonoSmall; color: Theme.textSecondary
        }
        Text {
            x: parent.width - width
            y: control.trackY + 12
            text: control.format(control.to)
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
                text: control.format(control.shownValue)
                font: Theme.fontMonoSmall; color: Theme.textPrimary
            }
        }
    }
}
