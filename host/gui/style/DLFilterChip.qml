import QtQuick
import QtQuick.Templates as T

// The params filter chips (All / Changed n / Pending n): a checkable Badge.
// `tone` colours the count when it is not zero (Unsaved, Pending).
T.AbstractButton {
    id: control

    property int tone: DLBadge.Neutral

    checkable: true
    implicitWidth: label.implicitWidth + 2 * Theme.space8
    implicitHeight: 20
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        radius: Theme.radiusSm
        color: control.checked ? Theme.bgSelected : control.hovered ? Theme.bgHover : Theme.bgSecondary
        border.width: control.visualFocus ? Theme.borderFocusWidth : 0
        border.color: Theme.borderFocus
    }
    contentItem: Text {
        id: label
        text: control.text
        font: Theme.fontCaption
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.checked ? Theme.accentText
             : control.tone === DLBadge.Unsaved ? Theme.stateUnsaved
             : control.tone === DLBadge.Pending ? Theme.statePending : Theme.textSecondary
    }
}
