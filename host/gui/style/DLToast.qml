import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T

// Figma Toast: Info, Ok or Fault, 380 px wide. Info and Ok dismiss themselves
// after `timeout` ms; a Fault toast stays until closed. The shell stacks them
// bottom right.
T.Control {
    id: control

    enum Tone { Info, Ok, Fault }

    property int tone: DLToast.Info
    property string text
    property int timeout: 4000

    signal dismissed()

    implicitWidth: 380
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    padding: Theme.space12
    Accessible.role: Accessible.AlertMessage
    Accessible.name: text

    Timer {
        running: control.visible && control.tone !== DLToast.Fault && control.timeout > 0
        interval: control.timeout
        onTriggered: control.dismissed()
    }

    contentItem: RowLayout {
        spacing: Theme.space8

        DLIcon {
            Layout.alignment: Qt.AlignTop | Qt.AlignLeft
            Layout.topMargin: 2
            name: ["info", "circle-check", "circle-x"][control.tone]
            color: [Theme.accentText, Theme.stateOk, Theme.stateFault][control.tone]
        }
        Text {
            Layout.fillWidth: true
            text: control.text
            font: Theme.fontBodyDefault
            color: Theme.textPrimary
            wrapMode: Text.Wrap
            lineHeightMode: Text.FixedHeight
            lineHeight: Theme.lineBodyDefault
        }
        T.AbstractButton {
            objectName: "closeButton"
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2
            implicitWidth: Theme.sizeIconSm
            implicitHeight: Theme.sizeIconSm
            focusPolicy: Qt.StrongFocus
            Accessible.name: qsTr("Dismiss")
            onClicked: control.dismissed()

            contentItem: DLIcon {
                name: "x"
                color: Theme.textTertiary
            }
        }
    }

    background: Rectangle {
        color: Theme.bgRaised
        radius: Theme.radiusMd
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault

        DLShadow {
            radius: parent.radius
        }
    }
}
