import QtQuick

// "Every refusal explains itself": wraps one control and shows `reason` in a
// tooltip while the control is disabled. A disabled item gets no hover events,
// so the hover is taken by this enabled wrapper instead.
//
//     DLReasonHover {
//         reason: qsTr("Disarm to save")
//         DLButton { text: qsTr("Save"); enabled: !armed }
//     }
Item {
    id: root

    property string reason
    readonly property Item control: children.length > 0 ? children[0] : null
    readonly property bool showing: control !== null && !control.enabled && reason.length > 0 && hover.hovered

    implicitWidth: control ? control.implicitWidth : 0
    implicitHeight: control ? control.implicitHeight : 0

    HoverHandler {
        id: hover
    }

    DLToolTip {
        visible: root.showing
        text: root.reason
    }
}
