import QtQuick
import QtQuick.Window
import DriveLink.Style

// Every DriveLink.Style control in its states, for comparison with the Figma
// G2 component sets. drivelink-style-gallery --out DIR renders it in Dark and
// Light. Hover cannot be forced offscreen; pressed states set `down`.
Window {
    id: window

    width: 1600
    height: page.implicitHeight
    visible: true
    color: Theme.bgApp
    title: "DriveLink style gallery"

    component Section: Column {
        property alias title: heading.text
        default property alias content: flow.data
        width: page.width - 2 * Theme.space32
        spacing: Theme.space12

        Text {
            id: heading
            font: Theme.fontLabelCaps
            color: Theme.textSecondary
        }
        Flow {
            id: flow
            width: parent.width
            spacing: Theme.space16
        }
    }

    Column {
        id: page
        width: window.width
        padding: Theme.space32
        spacing: Theme.space32

        Section {
            title: "BUTTON"
            Repeater {
                model: [DLButton.Primary, DLButton.Secondary, DLButton.Ghost, DLButton.Danger]
                Row {
                    required property int modelData
                    spacing: Theme.space8
                    DLButton { kind: parent.modelData; text: "Button" }
                    DLButton { kind: parent.modelData; text: "Pressed"; down: true }
                    DLButton { kind: parent.modelData; text: "Disabled"; enabled: false }
                    DLButton { kind: parent.modelData; text: "Icon"; iconName: "plus" }
                }
            }
        }

        Section {
            title: "ICON BUTTON"
            DLIconButton { iconName: "settings"; toolTipText: "Settings" }
            DLIconButton { iconName: "settings"; toolTipText: "Pressed"; down: true }
            DLIconButton { iconName: "settings"; toolTipText: "Disabled"; enabled: false }
            DLIconButton { size: DLIconButton.Sm; iconName: "settings"; toolTipText: "Small" }
            DLIconButton { size: DLIconButton.Sm; iconName: "pencil"; toolTipText: "Rename"; down: true }
        }

        Section {
            title: "ARM BUTTON"
            DLArmButton { mode: DLArmButton.Arm }
            DLArmButton { mode: DLArmButton.Arm; enabled: false }
            DLArmButton { mode: DLArmButton.Arming }
            DLArmButton { mode: DLArmButton.AckFault }
            DLArmButton { mode: DLArmButton.Disarm }
            DLArmButton { mode: DLArmButton.Disarm; down: true }
            DLArmButton { mode: DLArmButton.Disarm; enabled: false }
        }

        Section {
            title: "STATE CHIP"
            Repeater {
                model: 7
                DLStateChip { required property int index; status: index }
            }
        }

        Section {
            title: "BADGE"
            Repeater {
                model: [["record", DLBadge.Neutral], ["live", DLBadge.Accent], ["ROS driving", DLBadge.Armed],
                        ["disarms", DLBadge.Fault], ["applied", DLBadge.Ok], ["reboot", DLBadge.Pending],
                        ["unsaved", DLBadge.Unsaved], ["locked", DLBadge.Locked]]
                Column {
                    required property var modelData
                    spacing: Theme.space4
                    DLBadge { text: parent.modelData[0]; tone: parent.modelData[1] }
                    DLBadge { text: parent.modelData[0]; tone: parent.modelData[1]; size: DLBadge.Sm }
                }
            }
            DLBadge { text: "2 faults"; iconName: "triangle-alert" }
            DLBadge { text: "1 active"; iconName: "triangle-alert"; tone: DLBadge.Fault }
        }

        Section {
            title: "DEVICE CHIP"
            DLDeviceChip { name: "bench-rig"; deviceId: "a4cf123e9b01"; connection: "COM5 · 921600" }
            DLDeviceChip { link: DLDeviceChip.Agent; name: "bench-rig"; deviceId: "a4cf123e9b01"; connection: "agent 192.168.1.20" }
            DLDeviceChip { deviceId: "02444c000001"; connection: "tcp 127.0.0.1:8765" }
        }

        Section {
            title: "NAV RAIL, NAV ITEM"
            DLNavRail {
                id: rail
                height: 420
                model: [{ text: "Device", icon: "cpu" }, { text: "Layout", icon: "workflow" },
                        { text: "Tune", icon: "sliders-horizontal" }, { text: "Monitor", icon: "activity" }]
                footerModel: [{ text: "Settings", icon: "settings" }]
                onActivated: index => currentIndex = index
            }
            DLNavItem { text: "Device"; iconName: "cpu" }
            DLNavItem { text: "Device"; iconName: "cpu"; checked: true }
            DLNavItem { text: "Device"; iconName: "cpu"; enabled: false }
        }

        Section {
            title: "STATUS BAR"
            Column {
                spacing: Theme.space8
                width: page.width - 2 * Theme.space32
                Repeater {
                    model: [
                        { mode: DLStatusBar.NoDevice, reason: "No device connected" },
                        { mode: DLStatusBar.Loading, reason: "Loading the device model · 64 %" },
                        { mode: DLStatusBar.Disarmed, reason: "last: watchdog timeout · 18:20:11", faults: "2 faults", unsaved: true },
                        { mode: DLStatusBar.Arming, reason: "checking ARM conditions", faults: "2 faults" },
                        { mode: DLStatusBar.Armed, reason: "since 18:44:02", faults: "2 faults", unsaved: true, saveEnabled: false },
                        { mode: DLStatusBar.Fault, reason: "feedback.SignalLost · right_enc", faults: "1 active", active: true, pending: true },
                        { mode: DLStatusBar.SafeMode, reason: "3 crash boots · 0 instances", faults: "3 faults" },
                        { mode: DLStatusBar.LinkLost, reason: "Link lost · retry 3", faults: "2 faults", unsaved: true, saveEnabled: false }
                    ]
                    DLStatusBar {
                        required property var modelData
                        width: parent.width
                        mode: modelData.mode
                        deviceName: "bench-rig"
                        deviceId: "a4cf123e9b01"
                        connection: "COM5 · 921600"
                        reason: modelData.reason
                        faultsVisible: modelData.faults !== undefined
                        faultsText: modelData.faults || ""
                        faultsActive: modelData.active === true
                        unsaved: modelData.unsaved === true
                        saveEnabled: modelData.saveEnabled !== false
                        revertEnabled: modelData.saveEnabled !== false
                        pendingReboot: modelData.pending === true
                    }
                }
            }
        }

        Section {
            title: "BANNER"
            Column {
                spacing: Theme.space8
                width: page.width - 2 * Theme.space32
                DLBanner { width: parent.width; tone: DLBanner.Info; title: "First boot"; message: "No layout yet. Add instances, then save and reboot."; action1Text: "Add instance" }
                DLBanner { width: parent.width; tone: DLBanner.Pending; title: "Reboot pending"; message: "3 layout changes and 1 param apply after a reboot."; action1Text: "Review"; action2Text: "Save and reboot"; action3Text: "Discard all" }
                DLBanner { width: parent.width; tone: DLBanner.Armed; iconName: "radio"; title: "ROS is driving"; message: "ros_bridge holds the lease. Param edits apply live to a moving robot."; action1Text: "Request lease" }
                DLBanner { width: parent.width; tone: DLBanner.Fault; title: "Safe mode"; message: "Boot failed twice, so instances are not running. ARM is unavailable."; action1Text: "Exit safe mode" }
            }
        }

        Section {
            title: "INPUTS, CHECKS, PROGRESS"
            DLTextField { width: 260; text: "bench-left" }
            DLTextField { width: 260; placeholderText: "02:44:4c:00:00:01" }
            DLTextField { width: 260; text: "name	with tab"; error: true }
            DLComboBox { width: 180; model: [921600, 460800, 230400, 115200] }
            Column {
                spacing: Theme.space8
                DLCheckItem { width: 320; status: DLCheckItem.Pass; label: "Device metadata and status are current" }
                DLCheckItem { width: 320; status: DLCheckItem.Fail; label: "No fault latched"; detail: "Acknowledge the fault" }
                DLCheckItem { width: 320; status: DLCheckItem.Active; label: "Load descriptors, layout and status" }
                DLCheckItem { width: 320; status: DLCheckItem.Pending; label: "Command keepalives are ready" }
            }
            Column {
                spacing: Theme.space16
                DLProgressBar { width: 240; value: 0.4 }
                DLProgressBar { width: 240; value: 0.9 }
            }
        }

        Section {
            title: "TOAST, TOOLTIP"
            DLToast { tone: DLToast.Info; text: "Model reloaded: the descriptor hash changed."; timeout: 0 }
            DLToast { tone: DLToast.Ok; text: "Saved to flash."; timeout: 0 }
            DLToast { tone: DLToast.Fault; text: "ARM refused: fault latched (SignalLost on right_enc)." }
            Item {
                width: 260
                height: 64
                DLButton {
                    anchors.bottom: parent.bottom
                    text: "Locked"
                    DLToolTip { visible: true; delay: 0; text: "Locked while armed. Disarm to edit." }
                }
            }
        }

        Section {
            title: "DIALOG"
            Item {
                width: 500
                height: 220
                DLDialog {
                    visible: true
                    title: "Save to flash?"
                    text: "Writes the live params and layout to NVS on bench-rig, so they survive a reboot."
                    acceptText: "Save"
                }
            }
            Item {
                width: 500
                height: 220
                DLDialog {
                    visible: true
                    destructive: true
                    title: "Factory reset?"
                    text: "Erases the layout and every saved param on bench-rig, then reboots it."
                    acceptText: "Factory reset"
                }
            }
            Item {
                width: 500
                height: 250
                DLDialog {
                    visible: true
                    title: "Apply live while ROS drives?"
                    text: "ros_bridge holds the lease and the robot may be moving."
                    acceptText: "Apply live"
                    confirmationText: "Don't ask again for this connection"
                    confirmed: true
                }
            }
        }
    }
}
