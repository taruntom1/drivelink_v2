# Custom panels (H2f): one directory per module type, named after the type the
# device reports. Layout loads DriveLink/Modules/<type name>/Panel.qml from
# these resources. Module-specific GUI code lives only here.
qt_add_resources(drivelink_gui_shell drivelink_modules
  PREFIX /qt/qml/DriveLink/Modules
  FILES
    imu.bno055/Panel.qml
    imu.bno055/CalibrationPanel.qml
    imu.bno055/calibration.js
    imu.bno055/StatusBanner.qml
    imu.bno055/LevelsRow.qml
    imu.bno055/OrientationCard.qml
    imu.bno055/GuidanceCard.qml
    imu.bno055/FileCard.qml
    imu.bno055/OperationCard.qml
    imu.bno055/ApplyDialog.qml
    imu.mock/Panel.qml)
