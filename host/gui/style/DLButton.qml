import QtQuick
import QtQuick.Templates as T

// Figma Button: Primary, Secondary, Ghost and Danger. Disabled is 40 % opacity;
// wrap it in DLReasonHover to say why. The focus ring is drawn outside.
T.Button {
    id: control

    enum Kind { Primary, Secondary, Ghost, Danger }

    property int kind: DLButton.Secondary
    property string iconName
    property string toolTipText

    readonly property color foreground: kind === DLButton.Primary ? Theme.textOnAccent
                                      : kind === DLButton.Danger ? Theme.stateFault : Theme.textPrimary
    readonly property bool hot: enabled && hovered

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.sizeControl
    padding: 0
    leftPadding: Theme.space12
    rightPadding: Theme.space12
    spacing: Theme.space8
    font: Theme.fontBodyStrong
    opacity: enabled ? 1 : 0.4
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.spacing

            DLIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: control.iconName
                visible: name.length > 0
                color: control.foreground
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font: control.font
                color: control.foreground
            }
        }
    }

    background: Rectangle {
        implicitHeight: Theme.sizeControl
        radius: Theme.radiusMd
        color: {
            switch (control.kind) {
            case DLButton.Primary:
                return control.down ? Theme.accentPressed : control.hot ? Theme.accentHover : Theme.accentDefault
            case DLButton.Secondary:
                return control.down ? Theme.bgSecondaryPressed : control.hot ? Theme.bgSecondaryHover : Theme.bgSecondary
            case DLButton.Ghost:
                return control.down ? Theme.bgPressed : control.hot ? Theme.bgHover : "transparent"
            default:
                return control.down || control.hot ? Theme.stateFaultSubtle : "transparent"
            }
        }
        border.width: control.kind === DLButton.Secondary ? Theme.borderWidth
                    : control.kind === DLButton.Danger ? (control.down ? 2 : Theme.borderWidth) : 0
        border.color: control.kind === DLButton.Danger ? Theme.stateFault : Theme.borderDefault

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

    DLToolTip {
        visible: control.hovered && control.toolTipText.length > 0
        text: control.toolTipText
    }
}
