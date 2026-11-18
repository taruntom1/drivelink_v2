import QtQuick
import QtQuick.Templates as T

// Figma GainControl (G5): one gain of Tune's controller. Its name and
// description, the value field, the log slider over a window of ÷4..×4
// around the value at the start of a drag (re-centred on release; the window
// can be typed), ticks for the saved value and the shown runs' gains, the %
// steps, and a note. A gain of 0 disables the slider and the steps until a
// value is typed. It only draws and emits; the owner sends at most every
// 50 ms while dragging and once on release.
Rectangle {
    id: control

    property string name
    property string description
    property string unit
    property string valueText
    property real position: 0.5
    property string lowText
    property string midText
    property string highText
    property var ticks: []
    property string savedText
    property bool zero: false
    property bool sending: false
    property string problem

    signal sliderPressed()
    signal sliderMoved(real position)
    signal sliderReleased()
    signal valueTyped(string text)
    signal stepped(real factor)
    signal keyStepped(bool up, bool shift, bool ctrl)
    signal windowTyped(string low, string high)

    readonly property bool dragging: slider.pressed

    implicitWidth: 344
    implicitHeight: column.implicitHeight + 2 * Theme.space12
    radius: Theme.radiusMd
    color: Theme.bgSurface
    border.width: Theme.borderWidth
    border.color: Theme.borderDefault

    Column {
        id: column
        x: Theme.space12; y: Theme.space12
        width: parent.width - 2 * Theme.space12
        spacing: Theme.space12

        Item {
            width: parent.width; height: Theme.sizeControl
            Row {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - field.width - Theme.space8
                spacing: Theme.space8
                clip: true
                Text { id: nameText; text: control.name; font: Theme.fontBodyStrong; color: Theme.textPrimary }
                Text {
                    anchors.baseline: nameText.baseline
                    text: control.description; elide: Text.ElideRight
                    width: Math.max(0, parent.width - nameText.width - Theme.space8)
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
            }
            DLNumberField {
                id: field
                objectName: "gainValue"
                anchors.right: parent.right
                width: 132
                unit: control.unit
                status: control.problem.length > 0 ? DLNumberField.Rejected
                      : control.sending ? DLNumberField.Pending : DLNumberField.Normal
                // Sent on Enter; leaving the field drops the typed text.
                onAccepted: if (text !== control.valueText) control.valueTyped(text)
                onEditingFinished: Qt.callLater(() => text = control.valueText)
            }
            Binding { target: field; property: "text"; value: control.valueText; when: !field.activeFocus }
        }

        DLSlider {
            id: slider
            objectName: "gainSlider"
            width: parent.width
            scaleType: DLSlider.Log
            from: 0; to: 1; stepSize: 0
            snapMode: T.Slider.NoSnap
            enabled: !control.zero
            lowText: control.lowText; midText: control.midText; highText: control.highText
            bubbleText: control.valueText
            ticks: control.ticks
            onPressedChanged: pressed ? control.sliderPressed() : control.sliderReleased()
            onMoved: if (pressed) control.sliderMoved(value)
            // Arrows 1 %, Shift 10 %, Ctrl 0.1 % (not the slider's own steps).
            Keys.onPressed: function(event) {
                const up = event.key === Qt.Key_Up || event.key === Qt.Key_Right
                const down = event.key === Qt.Key_Down || event.key === Qt.Key_Left
                if (!up && !down) return
                event.accepted = true
                control.keyStepped(up, (event.modifiers & Qt.ShiftModifier) !== 0, (event.modifiers & Qt.ControlModifier) !== 0)
            }
        }
        Binding { target: slider; property: "value"; value: control.position; when: !slider.pressed }

        Item {
            width: parent.width; height: Theme.sizeControlSm
            Row {
                spacing: Theme.space4
                Repeater {
                    model: [{text: "÷2", factor: 0.5}, {text: "−10%", factor: 0.9}, {text: "+10%", factor: 1.1}, {text: "×2", factor: 2.0}]
                    delegate: DLButton {
                        required property var modelData
                        objectName: "gainStep_" + modelData.factor
                        height: Theme.sizeControlSm
                        kind: DLButton.Secondary
                        text: modelData.text
                        enabled: !control.zero
                        onClicked: control.stepped(modelData.factor)
                    }
                }
            }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space4
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: control.savedText.length > 0
                    text: qsTr("saved %1").arg(control.savedText)
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                DLIconButton {
                    objectName: "gainWindowEdit"
                    size: DLIconButton.Sm
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "pencil"
                    enabled: !control.zero
                    toolTipText: qsTr("Set the slider's window")
                    onClicked: {
                        windowRow.visible = true
                        low.text = control.lowText; high.text = control.highText
                        low.forceActiveFocus(); low.selectAll()
                    }
                }
            }
        }
        Row {
            id: windowRow
            objectName: "gainWindow"
            visible: false
            spacing: Theme.space8
            DLNumberField { id: low; width: 96; onAccepted: high.forceActiveFocus() }
            Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("to"); font: Theme.fontCaption; color: Theme.textSecondary }
            DLNumberField {
                id: high; width: 96
                onAccepted: { windowRow.visible = false; control.windowTyped(low.text, high.text) }
            }
            DLIconButton {
                size: DLIconButton.Sm; iconName: "x"; anchors.verticalCenter: parent.verticalCenter
                toolTipText: qsTr("Cancel"); onClicked: windowRow.visible = false
            }
        }

        Text {
            objectName: "gainNote"
            width: parent.width
            wrapMode: Text.Wrap
            text: control.problem.length > 0 ? control.problem
                : control.zero ? qsTr("Log slider and % steps need a non-zero value: type one first.")
                : control.dragging ? qsTr("Dragging: sends live at most every 50 ms, and once on release.")
                : qsTr("Arrows 1 % · Shift 10 % · Ctrl 0.1 %")
            font: Theme.fontCaption
            color: control.problem.length > 0 ? Theme.stateFault : control.dragging ? Theme.accentText : Theme.textSecondary
        }
    }
}
