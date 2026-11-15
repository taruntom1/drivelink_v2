qt_add_executable(drivelink-style-gallery main.cpp)
set_target_properties(drivelink-style-gallery PROPERTIES WIN32_EXECUTABLE FALSE)
qt_add_qml_module(drivelink-style-gallery
  URI DriveLink.Gallery
  VERSION 1.0
  QML_FILES Gallery.qml
)
target_link_libraries(drivelink-style-gallery PRIVATE drivelink_gui_style Qt6::Gui Qt6::Qml Qt6::Quick)
