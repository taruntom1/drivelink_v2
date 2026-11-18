import QtQuick

// A 0..3 level meter (Figma CalibrationLevel, G2): a title, "N / 3", three
// segments and a caption. level -1 is Unavailable: "— / 3", no filled
// segment, muted text. The whole card has a fixed height; the width comes
// from the parent. Generic: the owner supplies the words.
Rectangle {
    id: control

    property string title
    // 0..3, or -1 for Unavailable.
    property int level: -1
    property string caption

    readonly property bool unavailable: level < 0
    readonly property int filled: unavailable ? 0 : Math.min(level, 3)
    readonly property color fillColor: level >= 3 ? Theme.stateOk : Theme.accentDefault

    implicitWidth: 240
    implicitHeight: Theme.space12 + Theme.lineBodyDefault + Theme.space8 + 4 + Theme.space8 + Theme.lineCaption + Theme.space12
    color: Theme.bgSurface
    radius: Theme.radiusLg
    border.width: Theme.borderWidth
    border.color: Theme.borderDefault
    Accessible.role: Accessible.StaticText
    Accessible.name: title + ", " + valueText.text + ", " + caption

    Text {
        id: titleText
        objectName: "levelTitle"
        x: Theme.space16; y: Theme.space12
        width: parent.width - valueText.width - 3 * Theme.space16
        height: Theme.lineBodyDefault
        text: control.title
        font: Theme.fontBodyDefault
        color: control.unavailable ? Theme.textSecondary : Theme.textPrimary
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
    Text {
        id: valueText
        objectName: "levelValue"
        anchors.right: parent.right
        anchors.rightMargin: Theme.space16
        y: Theme.space12
        height: Theme.lineBodyDefault
        text: (control.unavailable ? "—" : control.level) + " / 3"
        font: Theme.fontMonoDefault
        color: control.unavailable ? Theme.textTertiary : Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
    }
    Row {
        x: Theme.space16
        y: Theme.space12 + Theme.lineBodyDefault + Theme.space8
        width: parent.width - 2 * Theme.space16
        spacing: Theme.space4
        Repeater {
            model: 3
            Rectangle {
                required property int index
                objectName: "levelSegment" + index
                readonly property bool on: index < control.filled
                width: (parent.width - 2 * parent.spacing) / 3
                height: 4
                radius: 2
                color: on ? control.fillColor : Theme.bgSecondary
            }
        }
    }
    Text {
        objectName: "levelCaption"
        x: Theme.space16
        y: parent.height - Theme.space12 - Theme.lineCaption
        width: parent.width - 2 * Theme.space16
        height: Theme.lineCaption
        text: control.caption
        font: Theme.fontCaption
        color: control.unavailable ? Theme.textTertiary : Theme.textSecondary
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
