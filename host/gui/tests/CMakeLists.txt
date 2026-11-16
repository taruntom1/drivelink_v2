# Offscreen Qt Quick tests of the DriveLink style, and a gallery smoke test.
find_package(Qt6 6.10 REQUIRED COMPONENTS QuickTest)
find_package(Qt6 6.10 REQUIRED COMPONENTS Test)
qt_add_executable(gui_backend_tests test_backend.cpp)
set_target_properties(gui_backend_tests PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(gui_backend_tests PRIVATE VDEV_PATH="$<TARGET_FILE:drivelink-vdev>")
target_link_libraries(gui_backend_tests PRIVATE drivelink_gui_backend drivelink_gui_shell Qt6::Test)
add_dependencies(gui_backend_tests drivelink-vdev)

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
set_tests_properties(gui-backend PROPERTIES TIMEOUT 180
  ENVIRONMENT_MODIFICATION "${gui_test_env};QTEST_FUNCTION_TIMEOUT=set:60000")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME gui-screenshots COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scenarios.py
  --gui $<TARGET_FILE:drivelink-gui> --vdev $<TARGET_FILE:drivelink-vdev>
  --out ${CMAKE_CURRENT_BINARY_DIR}/screenshots --smoke)
set_tests_properties(gui-screenshots PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION "${gui_test_env}")
