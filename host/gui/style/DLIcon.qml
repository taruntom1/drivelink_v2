import QtQuick

// A bundled Lucide icon (Figma Icon/<name>), recoloured with a Theme colour.
// Icons are 16 px in controls and 20 px in the nav rail, banners and ArmButton.
Image {
    id: root

    property string name
    property color color: Theme.textSecondary
    property real size: Theme.sizeIconSm
    // Rotates continuously, for loader-circle while something is in flight.
    property bool spinning: false

    width: size
    height: size
    sourceSize: Qt.size(size, size)
    fillMode: Image.PreserveAspectFit
    smooth: true
    // The provider takes rrggbb; a colour's string form ends with it.
    source: name.length > 0 ? "image://dlicon/" + name + "/" + root.color.toString().slice(-6) : ""
    Accessible.ignored: true

    RotationAnimator on rotation {
        running: root.spinning && root.visible
        from: 0
        to: 360
        duration: 1000
        loops: Animation.Infinite
    }
}
