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
            title: "LAYOUT: NUMBER FIELD, SWITCH, TABS, FILTER CHIPS, MENU"
            DLNumberField { width: 180; text: "0.85"; unit: "1/(rad/s)" }
            DLNumberField { width: 180; text: "4.0"; unit: "1/rad"; status: DLNumberField.Edited }
            DLNumberField { width: 180; text: "5000"; unit: "Hz"; status: DLNumberField.Clamped }
            DLNumberField { width: 180; text: "abc"; status: DLNumberField.Rejected }
            DLNumberField { width: 180; text: "14"; status: DLNumberField.Pending }
            DLNumberField { width: 180; text: "1000"; unit: "Hz"; status: DLNumberField.Locked; enabled: false }
            DLSwitch { checked: true; text: "realtime" }
            DLSwitch { checked: false; text: "realtime" }
            Row {
                DLTabButton { text: "Params"; checked: true }
                DLTabButton { text: "Calibration" }
            }
            Row {
                spacing: Theme.space8
                DLFilterChip { text: "All"; checked: true }
                DLFilterChip { text: "Changed 2"; tone: DLBadge.Unsaved }
                DLFilterChip { text: "Pending 1"; tone: DLBadge.Pending }
            }
            Rectangle {
                width: 260; height: 140
                color: Theme.bgRaised; radius: Theme.radiusMd
                border.width: Theme.borderWidth; border.color: Theme.borderDefault
                Column {
                    x: Theme.space4; y: Theme.space4; width: parent.width - 2 * Theme.space4
                    DLMenuItem { width: parent.width; text: "Instance"; detail: "8 of 16 instances" }
                    DLMenuItem { width: parent.width; text: "Loop"; detail: "4 of 4 loops: remove a loop first"; enabled: false }
                    DLMenuItem { width: parent.width; text: "Discard all" }
                }
            }
        }

        Section {
            title: "LAYOUT: TREE ITEM, PARAM ROW, REF PICKER"
            Column {
                width: 400
                spacing: Theme.space2
                DLTreeItem { width: 400; isLoop: true; label: "control"; rateHz: 1000; realtime: true; budget: 0.041; hasBudget: true; selected: true }
                DLTreeItem { width: 400; label: "left_drive"; typeName: "controller.cascade_pid"; depth: 1 }
                DLTreeItem { width: 400; label: "left_pwm"; typeName: "actuator.mock"; depth: 2 }
                DLTreeItem { width: 400; label: "aux_pwm"; typeName: "actuator.mock"; depth: 1; pending: "add"; health: 255 }
                DLTreeItem { width: 400; label: "right_enc"; typeName: "feedback.quadrature"; depth: 1; health: 1; healthText: "failed" }
                DLTreeItem { width: 400; isLoop: true; label: "fusion"; rateHz: 250; budget: 0.0075; hasBudget: true; pending: "change" }
                DLTreeItem { width: 400; isLoop: true; label: "sensor"; rateHz: 100; health: 4; healthText: "offline" }
            }
            Column {
                width: 900
                spacing: Theme.space8
                DLParamRow { width: 900; name: "vel.kp"; unit: "1/(rad/s)"; valueText: "0.85"; rangeText: "0 … 1000" }
                DLParamRow { width: 900; name: "vel.ki"; unit: "1/rad"; valueText: "4.0"; unsaved: true }
                DLParamRow { width: 900; name: "vel.d_cutoff"; unit: "Hz"; valueText: "5000"; editState: "clamped"; editNote: "Clamped by the device to 5000 (max)" }
                DLParamRow { width: 900; name: "pwm_gpio"; editor: "gpio"; valueText: "17"; editState: "rejected"; editText: "18"; editNote: "GPIO 18 is already used by right_enc (pin_a)"; rebootRequired: true }
                DLParamRow { width: 900; name: "priority"; valueText: "12"; pendingText: "14"; isPending: true; rebootRequired: true }
                DLParamRow { width: 900; name: "rate_hz"; unit: "Hz"; valueText: "1000"; locked: true }
                DLParamRow { width: 900; name: "mode"; editor: "enum"; enums: [{ value: 0, name: "open_loop" }, { value: 1, name: "velocity" }]; valueBits: 1 }
                DLParamRow { width: 900; name: "realtime"; editor: "bool"; valueBits: 1; rebootRequired: true }
            }
            Column {
                spacing: Theme.space8
                DLRefPicker { width: 360; chipText: "actuator"; valueText: "left_pwm" }
                DLRefPicker { width: 360; chipText: "feedback"; valueText: "Not wired"; status: DLRefPicker.Unset }
                DLRefPicker { width: 360; chipText: "f32"; valueText: "right_enc.position"; status: DLRefPicker.Pending }
                DLRefPicker { width: 360; chipText: "f32"; valueText: "imu.calibration_read"; status: DLRefPicker.Rejected }
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
