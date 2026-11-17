qt_add_library(drivelink_gui_shell STATIC)
qt_add_qml_module(drivelink_gui_shell URI DriveLink.Shell VERSION 1.0 NO_PLUGIN
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/DriveLink/Shell
  QML_FILES Shell.qml Card.qml InfoRow.qml ConnectPage.qml DevicePage.qml FaultsDrawer.qml OtaDialog.qml
  LayoutPage.qml LayoutDialogs.qml TargetRow.qml PendingChangeText.qml)
target_link_libraries(drivelink_gui_shell PUBLIC drivelink_gui_style Qt6::Qml Qt6::Quick)
qt_add_executable(drivelink-gui main.cpp)
# Diagnostics and screenshot failures also need stderr on Windows.
set_target_properties(drivelink-gui PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(drivelink-gui PRIVATE drivelink_gui_backend drivelink_gui_shell)
