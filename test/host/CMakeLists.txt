cmake_minimum_required(VERSION 3.16)

# Host-side unit tests for the pure-logic parts of the firmware: the wire
# codec, the protocol encoders/decoders (including diag's codec and slot
# table), the PID and the velocity estimator.
# These sources have no ESP-IDF or FreeRTOS dependency; shim/ provides the
# two IDF headers they still include (sdkconfig.h, esp_log.h).
#
#   cmake -S test/host -B test/host/build -G Ninja
#   cmake --build test/host/build
#   ctest --test-dir test/host/build --output-on-failure

project(drivelink_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++20, close to IDF's gnu++2b

set(COMPONENTS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components)

# Firmware sources under test. Add a file here only if it builds without
# ESP-IDF (beyond the shims).
set(FIRMWARE_SOURCES
    ${COMPONENTS_DIR}/pid/src/pid_controller.cpp
    ${COMPONENTS_DIR}/motor_controller/src/velocity_estimator.cpp
    ${COMPONENTS_DIR}/motor_manager/src/motor_protocol.cpp
    ${COMPONENTS_DIR}/imu_bmo_055/src/imu_protocol.cpp
    ${COMPONENTS_DIR}/time_sync/src/time_sync_protocol.cpp
    ${COMPONENTS_DIR}/diag/src/diag_protocol.cpp
    ${COMPONENTS_DIR}/diag/src/diag_table.cpp
)

set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_wire_codec.cpp
    tests/test_pid.cpp
    tests/test_velocity_estimator.cpp
    tests/test_motor_protocol.cpp
    tests/test_motor_validation.cpp
    tests/test_imu_protocol.cpp
    tests/test_time_sync_protocol.cpp
    tests/test_diag_protocol.cpp
    tests/test_diag_table.cpp
)

add_executable(host_tests ${TEST_SOURCES} ${FIRMWARE_SOURCES})

target_include_directories(host_tests PRIVATE
    shim
    ${COMPONENTS_DIR}/uart_comm/include
    ${COMPONENTS_DIR}/pid/include
    ${COMPONENTS_DIR}/motor_controller/include
    ${COMPONENTS_DIR}/motor_manager/include
    ${COMPONENTS_DIR}/motor_manager/priv_include
    ${COMPONENTS_DIR}/imu_bmo_055/include
    ${COMPONENTS_DIR}/imu_bmo_055/priv_include
    ${COMPONENTS_DIR}/time_sync/priv_include
    ${COMPONENTS_DIR}/diag/include
    ${COMPONENTS_DIR}/diag/priv_include
)
# Third-party headers: no warnings from them.
target_include_directories(host_tests SYSTEM PRIVATE
    third_party/doctest
    ${COMPONENTS_DIR}/imu_bmo_055/vendor
)

# Same warning set as the IDF build, and warnings are errors.
target_compile_options(host_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror
)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
