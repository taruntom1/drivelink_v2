# drivelink-gui (H2a-1: the DriveLink style, its gallery and tests).
# Qt 6.10 is the API floor (Ubuntu 26.04); Windows builds use Qt 6.12.0.
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Qml Quick QuickTemplates2 Svg)
qt_standard_project_setup(REQUIRES 6.10)

add_subdirectory(style)
add_subdirectory(gallery)
qt_add_library(drivelink_gui_backend STATIC
  backend/connection.cpp backend/connection.hpp backend/device_models.cpp backend/device_models.hpp
  backend/connection_internal.hpp backend/connection_ota.cpp backend/connection_layout.cpp backend/connection_live.cpp
  backend/layout_model.cpp backend/layout_model.hpp backend/live_model.cpp backend/live_model.hpp
  backend/command_model.cpp backend/command_model.hpp backend/command_view.cpp backend/command_view.hpp
  backend/connection_monitor.cpp backend/monitor_model.cpp backend/monitor_model.hpp backend/monitor_view.cpp backend/monitor_view.hpp
  backend/connection_tune.cpp backend/identify_model.cpp backend/identify_model.hpp backend/identify_view.cpp backend/identify_view.hpp backend/tune_model.cpp backend/tune_model.hpp backend/tune_session.cpp backend/tune_session.hpp backend/tune_view.cpp backend/tune_view.hpp
  backend/models.cpp backend/models.hpp backend/ota_model.cpp backend/ota_model.hpp
  backend/safety_keys.cpp backend/safety_keys.hpp backend/app_settings.cpp backend/app_settings.hpp)
target_include_directories(drivelink_gui_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/backend)
target_link_libraries(drivelink_gui_backend PUBLIC drivelink Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
# The full version (cmake/DriveLinkVersion.cmake, read at configure time) and
# the About texts: the repository licence and the third-party notices.
if(NOT DRIVELINK_VERSION_FULL)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/DriveLinkVersion.cmake)
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/drivelink_version.hpp
  CONTENT "#pragma once\n#define DRIVELINK_VERSION_FULL \"@DRIVELINK_VERSION_FULL@\"\n" @ONLY)
target_include_directories(drivelink_gui_backend PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
qt_add_resources(drivelink_gui_backend drivelink_about PREFIX /drivelink/about
  BASE ${CMAKE_CURRENT_SOURCE_DIR}/../.. FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE)
qt_add_resources(drivelink_gui_backend drivelink_notices PREFIX /drivelink/about
  BASE ${CMAKE_CURRENT_SOURCE_DIR}/notices FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/notices/THIRD_PARTY_NOTICES.txt)
add_subdirectory(app)
add_subdirectory(modules)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
