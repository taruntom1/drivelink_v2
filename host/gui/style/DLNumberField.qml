import QtQuick
import QtQuick.Templates as T

// Figma SpinBox as a text field (QML SpinBox is integer-only): the value,
// right-aligned with tabular numerals, and its unit inside the field. The
// border follows `status`: Edited (not sent), Pending, Clamped, Rejected,
// Locked. The owner parses and sends; this control only edits text.
T.TextField {
    id: control

    enum Status { Normal, Edited, Pending, Clamped, Rejected, Locked }

    property int status: DLNumberField.Normal
    property string unit

    readonly property color statusColor: [Theme.borderDefault, Theme.stateUnsaved, Theme.statePending,
                                          Theme.accentText, Theme.stateFault, Theme.stateLocked][status]

    implicitWidth: 200
    implicitHeight: Theme.sizeControl
    leftPadding: Theme.space12
    rightPadding: Theme.space12 + (unitText.visible ? unitText.implicitWidth + Theme.space4 : 0)
                  + (lockIcon.visible ? Theme.sizeIconSm + Theme.space4 : 0)
    horizontalAlignment: TextInput.AlignRight
    verticalAlignment: TextInput.AlignVCenter
    font: Theme.fontBodyNumeric
    color: Theme.textPrimary
    placeholderTextColor: Theme.textTertiary
    selectionColor: Theme.accentDefault
    selectedTextColor: Theme.textOnAccent
    selectByMouse: true
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true

    background: Rectangle {
        implicitHeight: Theme.sizeControl
        radius: Theme.radiusMd
        color: control.readOnly ? "transparent" : Theme.bgField
        border.width: control.activeFocus || control.status !== DLNumberField.Normal ? Theme.borderFocusWidth : Theme.borderWidth
        border.color: control.status !== DLNumberField.Normal && control.status !== DLNumberField.Locked ? control.statusColor
                    : control.activeFocus ? Theme.borderFocus
                    : control.hovered && !control.readOnly ? Theme.borderStrong : Theme.borderDefault
    }

    Text {
        id: unitText
        anchors.right: parent.right
        anchors.rightMargin: Theme.space12 + (lockIcon.visible ? Theme.sizeIconSm + Theme.space4 : 0)
        anchors.verticalCenter: parent.verticalCenter
        visible: control.unit.length > 0
        text: control.unit
        font: control.font
        color: Theme.textSecondary
    }
    DLIcon {
        id: lockIcon
        anchors.right: parent.right
        anchors.rightMargin: Theme.space12
        anchors.verticalCenter: parent.verticalCenter
        visible: control.status === DLNumberField.Locked
        name: "lock"
        color: Theme.stateLocked
    }
    Text {
        x: control.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        width: control.width - control.leftPadding - control.rightPadding
        horizontalAlignment: Text.AlignRight
        visible: control.length === 0 && control.preeditText.length === 0
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        elide: Text.ElideRight
    }
}
