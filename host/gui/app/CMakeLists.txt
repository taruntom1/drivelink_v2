qt_add_library(drivelink_gui_shell STATIC)
qt_add_qml_module(drivelink_gui_shell URI DriveLink.Shell VERSION 1.0 NO_PLUGIN
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/DriveLink/Shell
  QML_FILES Shell.qml Card.qml InfoRow.qml ConnectPage.qml DevicePage.qml FaultsDrawer.qml OtaDialog.qml
  LayoutPage.qml LayoutDialogs.qml TargetRow.qml PendingChangeText.qml ActionRow.qml MonitorPage.qml TunePage.qml IdentifyCards.qml
  # PlotItem draws Monitor's and Tune's plots (registered by qml_register_types_DriveLink_Shell).
  SOURCES plot_item.cpp plot_item.hpp)
target_include_directories(drivelink_gui_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(drivelink_gui_shell PUBLIC drivelink_gui_style drivelink_gui_backend drivelink_gui_modules Qt6::Qml Qt6::Quick)
qt_add_executable(drivelink-gui main.cpp)
# Diagnostics and screenshot failures also need stderr on Windows.
set_target_properties(drivelink-gui PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(drivelink-gui PRIVATE drivelink_gui_backend drivelink_gui_shell)
