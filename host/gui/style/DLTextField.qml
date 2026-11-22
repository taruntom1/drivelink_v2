import QtQuick
import QtQuick.Templates as T

// Figma TextField: Default, Focus, Error, Disabled. `error` draws the fault
// border; the message belongs under the field, owned by the screen.
T.TextField {
    id: control

    property bool error: false
    property bool monospace: false

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, 240)
    implicitHeight: Theme.sizeControl
    leftPadding: Theme.space12
    rightPadding: Theme.space12
    verticalAlignment: TextInput.AlignVCenter
    font: monospace ? Theme.fontMonoDefault : Theme.fontBodyDefault
    color: Theme.textPrimary
    placeholderTextColor: Theme.textTertiary
    selectionColor: Theme.accentDefault
    selectedTextColor: Theme.textOnAccent
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true

    background: Rectangle {
        implicitHeight: Theme.sizeControl
        radius: Theme.radiusMd
        color: Theme.bgField
        border.width: control.activeFocus || control.error ? Theme.borderFocusWidth : Theme.borderWidth
        border.color: control.error ? Theme.stateFault
                    : control.activeFocus ? Theme.borderFocus
                    : control.hovered ? Theme.borderStrong : Theme.borderDefault
    }

    Text {
        // Templates draw no placeholder; keep it under the text.
        x: control.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        width: control.width - control.leftPadding - control.rightPadding
        visible: control.length === 0 && control.preeditText.length === 0
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        elide: Text.ElideRight
    }
}
