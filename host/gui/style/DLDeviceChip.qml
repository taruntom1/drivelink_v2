import QtQuick
import QtQuick.Templates as T

// Figma DeviceChip: name, device id and connection, at the left of the status
// bar; clicking it opens Device info. A device without a name shows its id in
// the name slot. A 12-digit hex id (the factory MAC) is shown with colons.
T.AbstractButton {
    id: control

    enum Link { Serial, Agent }

    property int link: DLDeviceChip.Serial
    property string name
    property string deviceId
    property string connection

    readonly property string formattedId: /^[0-9a-fA-F]{12}$/.test(deviceId)
                                          ? deviceId.toLowerCase().match(/../g).join(":") : deviceId

    text: name.length > 0 ? name : formattedId
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: Theme.sizeControl
    padding: 0
    leftPadding: Theme.space12
    rightPadding: Theme.space12
    spacing: Theme.space8
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: [text, connection].filter(s => s.length > 0).join(", ")

    contentItem: Row {
        spacing: control.spacing
        clip: true

        DLIcon {
            anchors.verticalCenter: parent.verticalCenter
            name: control.link === DLDeviceChip.Agent ? "radio" : "usb"
        }
        Text {
            objectName: "name"
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.name.length > 0 ? Theme.fontBodyStrong : Theme.fontMonoDefault
            color: Theme.textPrimary
        }
        Text {
            objectName: "deviceId"
            anchors.verticalCenter: parent.verticalCenter
            visible: control.name.length > 0 && control.formattedId.length > 0
            text: control.formattedId
            font: Theme.fontMonoSmall
            color: Theme.textTertiary
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: control.connection.length > 0
            width: 1
            height: 16
            color: Theme.borderDefault
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: control.connection.length > 0
            text: control.connection
            font: Theme.fontCaption
            color: Theme.textSecondary
        }
    }

    background: Rectangle {
        radius: Theme.radiusMd
        color: control.down ? Theme.bgSecondaryPressed : control.hovered ? Theme.bgSecondaryHover : Theme.bgSecondary
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault

        Rectangle {
            visible: control.visualFocus
            anchors.fill: parent
            anchors.margins: -Theme.borderFocusWidth
            radius: parent.radius + Theme.borderFocusWidth
            color: "transparent"
            border.width: Theme.borderFocusWidth
            border.color: Theme.borderFocus
        }
    }
}
