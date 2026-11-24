import QtQuick
import QtQuick.Templates as T

// Figma Dialog: Default or Destructive (Danger accept button), 480 px wide,
// with an optional confirmation checkbox ("Don't ask again ...").
//
// It is not a Qt modal popup: a Qt modal dims and blocks the whole window,
// including the status bar, and DISARM must stay live above every modal.
// The shell (H2a-2) dims and blocks only the page under the status bar.
// Enter accepts a Default dialog; a Destructive one never accepts on Enter.
// Esc rejects (the shell also disarms on Esc).
T.Dialog {
    id: control

    property bool destructive: false
    property string text
    property string acceptText: qsTr("OK")
    property string rejectText: qsTr("Cancel")
    property string confirmationText
    property alias confirmed: confirmation.checked
    // Extra content between the body and the actions (a field, a list).
    property alias extra: slot.data
    property bool acceptEnabled: true
    // false: the accept button only emits acceptRequested and the dialog stays
    // open (a device request answers first; the owner closes it on success).
    property bool closeOnAccept: true
    signal acceptRequested()
    function requestAccept() { if (closeOnAccept) accept(); else acceptRequested() }

    anchors.centerIn: parent
    width: 480
    implicitWidth: 480
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    padding: Theme.space24
    modal: false
    dim: false
    focus: true
    closePolicy: T.Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.bgRaised
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault

        DLShadow {
            dialog: true
            radius: parent.radius
        }
    }

    contentItem: Column {
        spacing: Theme.space16
        focus: true
        Keys.onReturnPressed: if (!control.destructive && control.acceptEnabled) control.requestAccept()
        Keys.onEnterPressed: if (!control.destructive && control.acceptEnabled) control.requestAccept()

        Text {
            objectName: "title"
            width: parent.width
            text: control.title
            textFormat: Text.PlainText
            font: Theme.fontTitleSection
            color: Theme.textPrimary
            wrapMode: Text.Wrap
            lineHeightMode: Text.FixedHeight
            lineHeight: Theme.lineTitleSection
        }
        Text {
            objectName: "body"
            width: parent.width
            visible: text.length > 0
            text: control.text
            textFormat: Text.PlainText
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
            wrapMode: Text.Wrap
            lineHeightMode: Text.FixedHeight
            lineHeight: Theme.lineBodyDefault
        }
        Column {
            id: slot
            objectName: "extra"
            width: parent.width
            spacing: Theme.space8
            visible: children.length > 0
        }
        T.CheckBox {
            id: confirmation
            objectName: "confirmation"
            visible: control.confirmationText.length > 0
            text: control.confirmationText
            spacing: Theme.space8
            padding: 0
            font: Theme.fontBodyDefault
            implicitWidth: implicitContentWidth + leftPadding + rightPadding
            implicitHeight: 20
            leftPadding: indicator.width + spacing

            indicator: Rectangle {
                y: (confirmation.height - height) / 2
                implicitWidth: Theme.sizeIconSm
                implicitHeight: Theme.sizeIconSm
                radius: Theme.radiusSm
                color: confirmation.checked ? Theme.accentDefault : Theme.bgField
                border.width: confirmation.checked ? 0 : Theme.borderWidth
                border.color: Theme.borderStrong

                DLIcon {
                    anchors.centerIn: parent
                    visible: confirmation.checked
                    name: "check"
                    size: 12
                    color: Theme.textOnAccent
                }
            }
            contentItem: Text {
                verticalAlignment: Text.AlignVCenter
                text: confirmation.text
                font: confirmation.font
                color: Theme.textPrimary
            }
        }
        Item {
            width: parent.width
            implicitHeight: actions.implicitHeight

            Row {
                id: actions
                anchors.right: parent.right
                spacing: Theme.space8

                DLButton {
                    objectName: "rejectButton"
                    kind: DLButton.Secondary
                    text: control.rejectText
                    onClicked: control.reject()
                }
                DLButton {
                    objectName: "acceptButton"
                    kind: control.destructive ? DLButton.Danger : DLButton.Primary
                    enabled: control.acceptEnabled
                    text: control.acceptText
                    onClicked: control.requestAccept()
                }
            }
        }
    }
}
