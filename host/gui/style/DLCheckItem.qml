import QtQuick

// Figma CheckItem (G3): Pass, Fail, Active, Pending with a label and an
// optional detail line. Used for ARM checks, model loading and OTA steps.
Item {
    id: control

    enum Status { Pass, Fail, Active, Pending }

    property int status: DLCheckItem.Pending
    property string label
    property string detail

    readonly property var icons: ["circle-check", "circle-x", "loader-circle", "ellipsis"]
    readonly property var colors: [Theme.stateOk, Theme.stateFault, Theme.accentText, Theme.textTertiary]

    implicitWidth: 320
    implicitHeight: Math.max(Theme.sizeIconMd, column.implicitHeight)

    DLIcon {
        id: icon
        y: 1
        name: control.icons[control.status]
        color: control.colors[control.status]
        size: Theme.sizeIconMd
        spinning: control.status === DLCheckItem.Active
    }
    Column {
        id: column
        anchors.left: icon.right
        anchors.leftMargin: Theme.space8
        anchors.right: parent.right
        spacing: Theme.space2
        Text {
            objectName: "label"
            width: parent.width
            text: control.label
            font: Theme.fontBodyDefault
            color: control.status === DLCheckItem.Pending ? Theme.textSecondary : Theme.textPrimary
            wrapMode: Text.Wrap
        }
        Text {
            objectName: "detail"
            width: parent.width
            visible: text.length > 0
            text: control.detail
            font: Theme.fontCaption
            color: control.status === DLCheckItem.Fail ? Theme.stateFault : Theme.textSecondary
            wrapMode: Text.Wrap
        }
    }
}
