cmake_minimum_required(VERSION 3.16)

# Host-side unit tests for the pure-logic parts of the firmware: the
# protocol/ library (CRC, framing, frame scanner, wire codec, the generated
# definitions; its tests are protocol/test) and the protocol encoders/decoders (including diag's codec and slot
# table), the PID and the velocity estimator.
# These sources have no ESP-IDF or FreeRTOS dependency; shim/ provides the
# two IDF headers they still include (sdkconfig.h, esp_log.h).
#
#   cmake -S firmware/test/host -B firmware/test/host/build -G Ninja
#   cmake --build firmware/test/host/build
#   ctest --test-dir firmware/test/host/build --output-on-failure

project(drivelink_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++20, close to IDF's gnu++2b

set(COMPONENTS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(PROTOCOL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../protocol)

# The pure wire library, built as the ordinary host library. Warnings are
# errors here, like for the firmware sources below.
add_subdirectory(${PROTOCOL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/protocol)
target_compile_options(drivelink_protocol PRIVATE -Werror)

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

# Generator fixtures: the generator runs on tools/gen/tests/fixtures at build
# time, and test_defs.cpp includes the type headers it writes. That proves the
# generated headers compile with -Werror, and nothing extra is checked in.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../tools/gen)
set(GEN_FIXTURE_DEFS ${GEN_DIR}/tests/fixtures)
set(GEN_FIXTURE_ROOT ${CMAKE_CURRENT_BINARY_DIR}/gen_fixture)
set(GEN_FIXTURE_INCLUDE ${GEN_FIXTURE_ROOT}/protocol/include/drivelink/protocol/gen)
set(GEN_FIXTURE_HEADERS
    ${GEN_FIXTURE_INCLUDE}/type_fixture_full.hpp
    ${GEN_FIXTURE_INCLUDE}/type_fixture_sensor.hpp
)
file(GLOB GEN_SOURCES ${GEN_DIR}/*.py)
file(GLOB GEN_FIXTURE_FILES ${GEN_FIXTURE_DEFS}/*.yaml)
add_custom_command(
    OUTPUT ${GEN_FIXTURE_HEADERS}
    COMMAND ${Python3_EXECUTABLE} ${GEN_DIR}/gen.py --defs ${GEN_FIXTURE_DEFS} --out ${GEN_FIXTURE_ROOT}
    DEPENDS ${GEN_SOURCES} ${GEN_FIXTURE_FILES}
    COMMENT "Generating the test fixture headers"
    VERBATIM
)

set(TEST_SOURCES
    tests/test_main.cpp
    ${PROTOCOL_DIR}/test/test_crc.cpp
    ${PROTOCOL_DIR}/test/test_framing.cpp
    ${PROTOCOL_DIR}/test/test_scanner.cpp
    ${PROTOCOL_DIR}/test/test_wire_codec.cpp
    ${PROTOCOL_DIR}/test/test_defs.cpp
    tests/test_pid.cpp
    tests/test_velocity_estimator.cpp
    tests/test_motor_protocol.cpp
    tests/test_motor_validation.cpp
    tests/test_imu_protocol.cpp
    tests/test_time_sync_protocol.cpp
    tests/test_diag_protocol.cpp
    tests/test_diag_table.cpp
)

add_executable(host_tests ${TEST_SOURCES} ${FIRMWARE_SOURCES} ${GEN_FIXTURE_HEADERS})
target_link_libraries(host_tests PRIVATE drivelink::protocol)

target_include_directories(host_tests PRIVATE
    shim
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
# The generated fixture headers, written at build time.
target_include_directories(host_tests PRIVATE ${GEN_FIXTURE_INCLUDE})
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
