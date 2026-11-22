# Offscreen Qt Quick tests of the DriveLink style, and a gallery smoke test.
find_package(Qt6 6.10 REQUIRED COMPONENTS QuickTest)
find_package(Qt6 6.10 REQUIRED COMPONENTS Test Network)
qt_add_executable(gui_backend_tests test_backend.cpp test_support.hpp)
set_target_properties(gui_backend_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_backend_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_backend_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test Qt6::Network)
add_dependencies(gui_backend_tests drivelink-vdev)
# One translation unit of many lambdas: past the default COFF section count.
if(MINGW)
  target_compile_options(gui_backend_tests PRIVATE -Wa,-mbig-obj)
endif()

# Monitor (H2d): pure rules, the view, groups on vdev, the page and the plot item.
qt_add_executable(gui_monitor_tests test_monitor.cpp test_support.hpp)
set_target_properties(gui_monitor_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_monitor_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_monitor_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test)
add_dependencies(gui_monitor_tests drivelink-vdev)

# Tune (H2e): pure rules, the view, runs on vdev and the page in the shell.
qt_add_executable(gui_tune_tests test_tune.cpp test_support.hpp)
set_target_properties(gui_tune_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_tune_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_tune_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test)
add_dependencies(gui_tune_tests drivelink-vdev)

# Custom panels (H2f): the generic panel surface and the IMU calibration
# panel in the shell on vdev's imu.mock; benchCalibration for a device.
qt_add_executable(gui_panel_tests test_panels.cpp test_support.hpp)
set_target_properties(gui_panel_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_panel_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_panel_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test)
add_dependencies(gui_panel_tests drivelink-vdev)

# Settings and About (H2g): AppSettings and the Settings page in the shell.
qt_add_executable(gui_settings_tests test_settings.cpp test_support.hpp)
set_target_properties(gui_settings_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_settings_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_settings_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test)
add_dependencies(gui_settings_tests drivelink-vdev)

# Remote mode (H3b-1): the pure rules on both OSes; through drivelink-agent
# and vdev where the agent builds (Linux).
qt_add_executable(gui_remote_tests test_remote.cpp test_support.hpp)
set_target_properties(gui_remote_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_remote_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_remote_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test)
add_dependencies(gui_remote_tests drivelink-vdev)
if(DRIVELINK_BUILD_AGENT)
  target_compile_definitions(gui_remote_tests PRIVATE AGENT_PATH="$<TARGET_FILE:drivelink-agent>")
  add_dependencies(gui_remote_tests drivelink-agent)
endif()

qt_add_executable(gui_quick_tests tst_main.cpp)
set_target_properties(gui_quick_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_quick_tests PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gui_quick_tests PRIVATE drivelink_gui_style Qt6::QuickTest Qt6::Qml Qt6::Quick)

# Offscreen and software rendering on both OSes; Windows also needs Qt's DLLs.
set(gui_test_env
  "QT_FORCE_STDERR_LOGGING=set:1"
  "QT_QPA_PLATFORM=set:offscreen"
  "QT_QUICK_BACKEND=set:software")
if(WIN32)
  list(APPEND gui_test_env "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

add_test(NAME gui-quick COMMAND gui_quick_tests)
set_tests_properties(gui-quick PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION "${gui_test_env}")

add_test(NAME gui-gallery
  COMMAND drivelink-style-gallery --out ${CMAKE_CURRENT_BINARY_DIR}/gallery)
set_tests_properties(gui-gallery PROPERTIES TIMEOUT 60 ENVIRONMENT_MODIFICATION "${gui_test_env}"
  PASS_REGULAR_EXPRESSION "gallery-light\\.png [0-9]+x[0-9]+")
add_test(NAME gui-backend COMMAND gui_backend_tests)
set_tests_properties(gui-backend PROPERTIES TIMEOUT 300
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:60000")
add_test(NAME gui-monitor COMMAND gui_monitor_tests)
set_tests_properties(gui-monitor PROPERTIES TIMEOUT 300
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:60000")
add_test(NAME gui-panels COMMAND gui_panel_tests)
set_tests_properties(gui-panels PROPERTIES TIMEOUT 300
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:90000")
add_test(NAME gui-remote COMMAND gui_remote_tests)
set_tests_properties(gui-remote PROPERTIES TIMEOUT 240
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:90000")
add_test(NAME gui-settings COMMAND gui_settings_tests)
set_tests_properties(gui-settings PROPERTIES TIMEOUT 120
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:60000")
add_test(NAME gui-tune COMMAND gui_tune_tests)
set_tests_properties(gui-tune PROPERTIES TIMEOUT 300
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:90000")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME gui-screenshots COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scenarios.py
  --gui $<TARGET_FILE:drivelink-gui> --vdev $<TARGET_FILE:drivelink-vdev>
  --out ${CMAKE_CURRENT_BINARY_DIR}/screenshots --smoke)
set_tests_properties(gui-screenshots PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION "${gui_test_env}")
