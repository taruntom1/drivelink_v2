import QtQuick

// Figma CommandControl: Released, Dragging, Latched. One commanded input:
// its name and instance, Latch, the symmetric slider (−limit … +limit within
// the descriptor range), the value field (editable only while latched), the
// limit with an edit button, and a state line. It only draws and emits; the
// owner applies the hold rule and sends.
Rectangle {
    id: control

    property string name
    property string owner
    property string unit
    property real value: 0
    property real from: -1
    property real to: 1
    property real limit: 1
    property real bound: 1
    property real stepSize: 0.01
    property int decimals: 2
    property bool latched: false
    property bool held: false
    property bool ramping: false
    // A live param write or a Tune run on this instance is in flight: held at 0.
    property bool writing: false
    // Commands are accepted (armed by this window); otherwise everything is
    // shown at 0 and only the limit and Remove work.
    property bool commandEnabled: false
    // The last refusal of a typed value or limit, shown under the field.
    property string problem

    signal sliderPressed()
    signal sliderMoved(real value)
    signal sliderReleased()
    signal latchToggled(bool on)
    signal valueTyped(string text)
    signal limitTyped(string text)
    signal removeRequested()

    readonly property bool dragging: slider.pressed
    function format(v) { return slider.format(v) }

    implicitWidth: 344
    implicitHeight: column.implicitHeight + 2 * Theme.space12
    radius: Theme.radiusMd
    color: Theme.bgSurface
    border.width: Theme.borderWidth
    border.color: control.latched && control.value !== 0 ? Theme.stateArmed : Theme.borderDefault

    Column {
        id: column
        x: Theme.space12; y: Theme.space12
        width: parent.width - 2 * Theme.space12
        spacing: Theme.space16

        Item {
            width: parent.width; height: 20
            Row {
                spacing: Theme.space8
                anchors.verticalCenter: parent.verticalCenter
                Text { text: control.name; font: Theme.fontBodyStrong; color: Theme.textPrimary }
                Text {
                    anchors.baseline: parent.children[0].baseline
                    text: control.owner; font: Theme.fontCaption; color: Theme.textSecondary
                }
            }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space8
                DLSwitch {
                    objectName: "commandLatch"
                    text: qsTr("Latch")
                    enabled: control.commandEnabled && !control.writing
                    checked: control.latched
                    onToggled: control.latchToggled(checked)
                }
                DLIconButton {
                    objectName: "commandRemove"
                    size: DLIconButton.Sm
                    iconName: "x"
                    toolTipText: qsTr("Remove: returns the input to 0")
                    onClicked: control.removeRequested()
                }
            }
        }

        DLSlider {
            id: slider
            objectName: "commandSlider"
            width: parent.width
            from: control.from
            to: control.to
            stepSize: control.stepSize
            decimals: control.decimals
            shownValue: control.value
            enabled: control.commandEnabled && !control.writing
            // Only a held handle commands (dead-man): no keyboard steps.
            focusPolicy: Qt.NoFocus
            onPressedChanged: pressed ? control.sliderPressed() : control.sliderReleased()
            // A move without a press never shows a value that was not sent.
            onMoved: pressed ? control.sliderMoved(value) : (value = control.value)
        }
        // While pressed the slider owns its value; otherwise it shows the
        // commanded one (a ramp, a typed value, Hold all).
        Binding { target: slider; property: "value"; value: control.value; when: !slider.pressed }

        Item {
            width: parent.width; height: Theme.sizeControl
            DLNumberField {
                id: field
                objectName: "commandValue"
                width: 140
                unit: control.unit
                readOnly: !(control.commandEnabled && control.latched && !control.writing)
                status: control.problem.length > 0 ? DLNumberField.Rejected : DLNumberField.Normal
                // Sent on Enter only: leaving the field (a click on Hold all,
                // Latch or Remove) drops the typed text. Afterwards the field
                // shows the value applied (clamped to the limit).
                onAccepted: if (!readOnly && text !== control.format(control.value)) control.valueTyped(text)
                // After `accepted`, which can follow editingFinished.
                onEditingFinished: Qt.callLater(() => text = control.format(control.value))
            }
            Binding { target: field; property: "text"; value: control.format(control.value); when: !field.activeFocus }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space8
                Text {
                    visible: !limitField.visible
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("limit ±%1").arg(Math.abs(control.limit).toFixed(control.decimals))
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                DLNumberField {
                    id: limitField
                    objectName: "commandLimitField"
                    visible: false
                    width: 96
                    onAccepted: { visible = false; control.limitTyped(text) }
                    onActiveFocusChanged: if (!activeFocus) visible = false
                }
                DLIconButton {
                    objectName: "commandLimitEdit"
                    size: DLIconButton.Sm
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "pencil"
                    toolTipText: qsTr("Change the limit (up to ±%1)").arg(control.bound.toFixed(control.decimals))
                    onClicked: {
                        limitField.text = Math.abs(control.limit).toFixed(control.decimals)
                        limitField.visible = true
                        limitField.forceActiveFocus()
                        limitField.selectAll()
                    }
                }
            }
        }

        Text {
            objectName: "commandNote"
            width: parent.width
            wrapMode: Text.Wrap
            readonly property bool running: control.latched && control.value !== 0
            text: control.problem.length > 0 ? control.problem
                : !control.commandEnabled ? qsTr("Release returns to 0")
                : control.writing ? qsTr("Held at 0 during a param write or a Tune run on this instance")
                : control.held ? qsTr("Sending · returns to 0 on release")
                : control.ramping ? qsTr("Returning to 0")
                : running ? qsTr("Latched at %1 · keeps running until you unlatch, Remove or Hold all")
                              .arg(control.format(control.value) + (control.unit.length > 0 ? " " + control.unit : ""))
                : control.latched ? qsTr("Latched · drag or type a value; it stays after release")
                : qsTr("Release returns to 0")
            font: Theme.fontCaption
            color: control.problem.length > 0 ? Theme.stateFault : running ? Theme.stateArmed : Theme.textSecondary
        }
    }
}
