# Custom panels (H2f): one directory per module type, named after the type the
# device reports. Layout loads DriveLink/Modules/<type name>/Panel.qml from
# these resources; a panel's C++ logic is registered by
# registerModulePanels(). Module-specific GUI code lives only here.
qt_add_library(drivelink_gui_modules STATIC
  register.cpp register.hpp
  imu.bno055/calibration.cpp imu.bno055/calibration.hpp
  imu.bno055/calibration_qml.cpp imu.bno055/calibration_qml.hpp)
target_include_directories(drivelink_gui_modules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drivelink_gui_modules PUBLIC Qt6::Core Qt6::Qml)
qt_add_resources(drivelink_gui_modules drivelink_modules
  PREFIX /qt/qml/DriveLink/Modules
  FILES
    imu.bno055/Panel.qml
    imu.bno055/CalibrationPanel.qml
    imu.bno055/StatusBanner.qml
    imu.bno055/LevelsRow.qml
    imu.bno055/OrientationCard.qml
    imu.bno055/GuidanceCard.qml
    imu.bno055/FileCard.qml
    imu.bno055/OperationCard.qml
    imu.bno055/ApplyDialog.qml
    imu.mock/Panel.qml)
