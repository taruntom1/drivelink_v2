import QtQuick

// A small isometric orientation drawing (Figma OrientationPreview, G2): a
// framed area with a title and a state tag, three axes drawn from the centre
// with their labels, a faint rhombus for the X-Y plane, and a caption.
// `axes` is [{axis, x, y, depth}] with x and y in [-1, 1] (y down), farthest
// first, as the owner projected them. No axes, or state "unavailable": no
// drawing, just "No orientation data". Stale draws everything muted and
// dashed. Generic: the owner supplies the numbers and the words.
Rectangle {
    id: control

    property var axes: []
    // "live" | "stale" | "unavailable"
    property string state: "unavailable"
    property string title: qsTr("Orientation")
    property string caption
    property string emptyText: qsTr("No orientation data")

    readonly property bool drawn: state !== "unavailable" && axes !== null && axes !== undefined && axes.length > 0
    readonly property bool stale: state === "stale"
    readonly property color lineColor: stale ? Theme.textTertiary : Theme.accentDefault
    readonly property color labelColor: stale ? Theme.textSecondary : Theme.accentText
    readonly property var axisOf: {
        var by = ({})
        if (axes) for (var i = 0; i < axes.length; ++i) by[axes[i].axis] = axes[i]
        return by
    }
    // The drawing area: between the header and the caption.
    readonly property real drawTop: Theme.space12 + Theme.lineBodyDefault + Theme.space8
    readonly property real drawBottom: height - Theme.space12 - captionText.height - Theme.space8
    readonly property real cx: width / 2
    readonly property real cy: (drawTop + drawBottom) / 2
    readonly property real unit: Math.max(10, Math.min(width / 2 - Theme.space24, (drawBottom - drawTop) / 2 - Theme.space4))

    implicitWidth: 320
    implicitHeight: 200
    color: Theme.bgRaised
    radius: Theme.radiusLg
    border.width: Theme.borderWidth
    border.color: Theme.borderDefault
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: title + ", " + (drawn ? state : emptyText)

    // One straight line from (x1, y1) to (x2, y2), optionally dashed.
    component Seg: Item {
        id: seg
        property real x1
        property real y1
        property real x2
        property real y2
        property color color: Theme.accentDefault
        property real thickness: 2
        property bool dashed: false
        readonly property real length: Math.hypot(x2 - x1, y2 - y1)
        x: x1; y: y1 - thickness / 2
        width: length; height: thickness
        transformOrigin: Item.Left
        rotation: Math.atan2(y2 - y1, x2 - x1) * 180 / Math.PI
        antialiasing: true
        Rectangle {
            visible: !seg.dashed
            anchors.fill: parent
            radius: seg.thickness / 2
            color: seg.color
            antialiasing: true
        }
        Repeater {
            model: seg.dashed ? Math.ceil(seg.length / 8) : 0
            Rectangle {
                required property int index
                x: index * 8
                width: Math.min(5, seg.length - index * 8)
                height: seg.thickness
                color: seg.color
            }
        }
    }

    Text {
        objectName: "previewTitle"
        x: Theme.space16; y: Theme.space12
        height: Theme.lineBodyDefault
        text: control.title
        font: Theme.fontBodyDefault
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
    }
    Text {
        objectName: "previewTag"
        anchors.right: parent.right; anchors.rightMargin: Theme.space16
        y: Theme.space12
        height: Theme.lineBodyDefault
        visible: control.state === "live" || control.state === "stale"
        text: control.state === "live" ? qsTr("LIVE") : qsTr("STALE")
        font: Theme.fontLabelCaps
        color: control.state === "live" ? Theme.accentText : Theme.stateUnsaved
        verticalAlignment: Text.AlignVCenter
    }

    Item {
        objectName: "previewDrawing"
        anchors.fill: parent
        visible: control.drawn

        // The sensor's X-Y plane (half a unit each way).
        Repeater {
            model: control.drawn && control.axisOf.X && control.axisOf.Y ? 4 : 0
            Seg {
                required property int index
                readonly property var ax: control.axisOf.X
                readonly property var ay: control.axisOf.Y
                readonly property var sa: [[1, 1], [1, -1], [-1, -1], [-1, 1]][index]
                readonly property var sb: [[1, -1], [-1, -1], [-1, 1], [1, 1]][index]
                x1: control.cx + 0.5 * control.unit * (sa[0] * ax.x + sa[1] * ay.x)
                y1: control.cy + 0.5 * control.unit * (sa[0] * ax.y + sa[1] * ay.y)
                x2: control.cx + 0.5 * control.unit * (sb[0] * ax.x + sb[1] * ay.x)
                y2: control.cy + 0.5 * control.unit * (sb[0] * ax.y + sb[1] * ay.y)
                thickness: 1
                color: control.stale ? Theme.borderStrong : Theme.accentDefault
                opacity: 0.6
                dashed: control.stale
            }
        }
        Repeater {
            model: control.drawn ? control.axes : []
            Seg {
                required property var modelData
                x1: control.cx; y1: control.cy
                x2: control.cx + modelData.x * control.unit
                y2: control.cy + modelData.y * control.unit
                color: control.lineColor
                dashed: control.stale
            }
        }
        Repeater {
            model: control.drawn ? control.axes : []
            Text {
                required property var modelData
                objectName: "axisLabel"
                x: control.cx + modelData.x * control.unit * 1.12 - width / 2
                y: control.cy + modelData.y * control.unit * 1.12 - height / 2
                text: modelData.axis
                font: Theme.fontMonoSmall
                color: control.labelColor
            }
        }
    }

    Text {
        objectName: "previewEmpty"
        anchors.horizontalCenter: parent.horizontalCenter
        y: control.drawTop + (control.drawBottom - control.drawTop - height) / 2
        visible: !control.drawn
        text: control.emptyText
        font: Theme.fontBodyDefault
        color: Theme.textSecondary
    }
    Text {
        id: captionText
        objectName: "previewCaption"
        x: Theme.space16
        y: parent.height - Theme.space12 - height
        width: parent.width - 2 * Theme.space16
        height: Math.max(Theme.lineCaption, implicitHeight)
        text: control.caption
        font: Theme.fontCaption
        color: Theme.textSecondary
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
