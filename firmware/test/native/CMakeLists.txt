cmake_minimum_required(VERSION 3.16)

# Native unit tests: everything that builds without ESP-IDF, run on the host.
#   - the protocol/ library (CRC, framing, frame scanner, wire codec, the
#     generated definitions; its tests are protocol/test)
#   - core_os's native backend and the pure core (registry, bind API, values,
#     loops, the param service, the layout, NVS persistence, safe mode and the
#     boot), with the generator's fixture types as test module types
#   - the legacy protocol codecs (motor, IMU, time_sync, diag and its slot
#     table), the PID and the velocity estimator
# The legacy sources include two IDF headers; shim/ provides them (sdkconfig.h,
# esp_log.h). The core includes neither.
#
#   cmake -S firmware/test/native -B firmware/test/native/build -G Ninja
#   cmake --build firmware/test/native/build
#   ctest --test-dir firmware/test/native/build --output-on-failure

project(drivelink_native_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++20, close to IDF's gnu++2b

set(COMPONENTS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(PROTOCOL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../protocol)

find_package(Threads REQUIRED)

# The pure wire library, built as the ordinary host library. Warnings are
# errors here, like for the firmware sources below.
add_subdirectory(${PROTOCOL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/protocol)
target_compile_options(drivelink_protocol PRIVATE -Werror)

# core_os, native backend (backend/native), and the pure core: every core
# source except src/idf/, which reads Kconfig.
set(CORE_SOURCES
    ${COMPONENTS_DIR}/core_os/backend/native/src/core_os_native.cpp
    ${COMPONENTS_DIR}/core_os/backend/native/src/kv_store_native.cpp
    ${COMPONENTS_DIR}/core_os/backend/native/src/native_file.cpp
    ${COMPONENTS_DIR}/core/src/config_blob.cpp
    ${COMPONENTS_DIR}/core/src/command_service.cpp
    ${COMPONENTS_DIR}/mod_mock/src/modules.cpp
    ${COMPONENTS_DIR}/core/src/core.cpp
    ${COMPONENTS_DIR}/core/src/crc32.cpp
    ${COMPONENTS_DIR}/core/src/instance.cpp
    ${COMPONENTS_DIR}/core/src/layout_validation.cpp
    ${COMPONENTS_DIR}/core/src/loop.cpp
    ${COMPONENTS_DIR}/core/src/param_service.cpp
    ${COMPONENTS_DIR}/core/src/pending_layout.cpp
    ${COMPONENTS_DIR}/core/src/persistence.cpp
    ${COMPONENTS_DIR}/core/src/registry.cpp
    ${COMPONENTS_DIR}/core/src/value.cpp
    ${COMPONENTS_DIR}/core/src/wiring_order.cpp
)

# Legacy firmware sources under test. Add a file here only if it builds
# without ESP-IDF (beyond the shims).
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
# time. test_defs.cpp includes the type headers it writes, and the core tests
# use the fixture types as test module types. That proves the generated
# headers compile with -Werror, and nothing extra is checked in. (The fixture
# type_system.hpp is written too but never included: the core uses the real
# one, through drivelink/protocol/gen/.)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../tools/gen)
set(GEN_FIXTURE_DEFS ${GEN_DIR}/tests/fixtures)
set(GEN_FIXTURE_ROOT ${CMAKE_CURRENT_BINARY_DIR}/gen_fixture)
set(GEN_FIXTURE_INCLUDE ${GEN_FIXTURE_ROOT}/protocol/include/drivelink/protocol/gen)
set(GEN_FIXTURE_HEADERS
    ${GEN_FIXTURE_INCLUDE}/type_fixture_full.hpp
    ${GEN_FIXTURE_INCLUDE}/type_fixture_sensor.hpp
    ${GEN_FIXTURE_INCLUDE}/type_actuator_fixture.hpp
    ${GEN_FIXTURE_INCLUDE}/type_feedback_fixture.hpp
    ${GEN_FIXTURE_INCLUDE}/type_fixture_wired.hpp
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
    tests/test_core_commands.cpp
    tests/test_core_os.cpp
    tests/test_core_registry.cpp
    tests/test_core_bind.cpp
    tests/test_core_value.cpp
    tests/test_core_params.cpp
    tests/test_core_loop.cpp
    tests/test_core_threaded.cpp
    tests/test_core_blob.cpp
    tests/test_core_persistence.cpp
    tests/test_core_layout.cpp
    tests/test_core_boot.cpp
    tests/test_core_safe_mode.cpp
    tests/test_pid.cpp
    tests/test_velocity_estimator.cpp
    tests/test_motor_protocol.cpp
    tests/test_motor_validation.cpp
    tests/test_imu_protocol.cpp
    tests/test_time_sync_protocol.cpp
    tests/test_diag_protocol.cpp
    tests/test_diag_table.cpp
)

add_executable(native_tests ${TEST_SOURCES} ${CORE_SOURCES} ${FIRMWARE_SOURCES} ${GEN_FIXTURE_HEADERS})
target_link_libraries(native_tests PRIVATE drivelink::protocol Threads::Threads)

target_include_directories(native_tests PRIVATE
    shim
    ${COMPONENTS_DIR}/core_os/include
    ${COMPONENTS_DIR}/core_os/backend/native/include
    ${COMPONENTS_DIR}/core/include
    ${COMPONENTS_DIR}/mod_mock/include
    ${COMPONENTS_DIR}/pid/include
    ${COMPONENTS_DIR}/motor_controller/include
    ${COMPONENTS_DIR}/motor_manager/include
    ${COMPONENTS_DIR}/motor_manager/priv_include
    ${COMPONENTS_DIR}/imu_bmo_055/include
    ${COMPONENTS_DIR}/imu_bmo_055/priv_include
    ${COMPONENTS_DIR}/time_sync/priv_include
    ${COMPONENTS_DIR}/diag/include
    ${COMPONENTS_DIR}/diag/priv_include
    tests
)
# The generated fixture headers, written at build time.
target_include_directories(native_tests PRIVATE ${GEN_FIXTURE_INCLUDE})
# Third-party headers: no warnings from them.
target_include_directories(native_tests SYSTEM PRIVATE
    third_party/doctest
    ${COMPONENTS_DIR}/imu_bmo_055/vendor
)

# Same warning set as the IDF build, and warnings are errors.
target_compile_options(native_tests PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror
)

enable_testing()
add_test(NAME native_tests COMMAND native_tests)
