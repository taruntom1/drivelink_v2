# drivelink-gui (H2a-1: the DriveLink style, its gallery and tests).
# Qt 6.10 is the API floor (Ubuntu 26.04); Windows builds use Qt 6.12.0.
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Qml Quick QuickTemplates2 Svg)
qt_standard_project_setup(REQUIRES 6.10)

add_subdirectory(style)
add_subdirectory(gallery)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
