set(COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../../firmware/components)
set(CORE_SOURCES
  config_blob command_service group_service source_service core safety crc32 instance layout_validation loop loop_instance
  param_service pending_layout persistence registry value wiring_order)
set(SOURCES)
foreach(source IN LISTS CORE_SOURCES)
  list(APPEND SOURCES ${COMPONENTS}/core/src/${source}.cpp)
endforeach()
add_library(drivelink_native_core STATIC ${SOURCES}
  ${COMPONENTS}/mod_mock/src/modules.cpp
  ${COMPONENTS}/mod_imu_bno055/src/state.cpp
  ${COMPONENTS}/mod_motor/src/controller.cpp
  ${COMPONENTS}/mod_motor/src/velocity_estimator.cpp
  ${COMPONENTS}/core_os/backend/native/src/realtime_native.cpp
    ${COMPONENTS}/core_os/backend/native/src/core_os_native.cpp
  ${COMPONENTS}/core_os/backend/native/src/periodic_timer_native.cpp
  ${COMPONENTS}/core_os/backend/native/src/kv_store_native.cpp
  ${COMPONENTS}/core_os/backend/native/src/native_file.cpp
  ${COMPONENTS}/diag/src/diag_protocol.cpp
  ${COMPONENTS}/diag/src/diag_table.cpp)
target_include_directories(drivelink_native_core PUBLIC
  ${COMPONENTS}/core/include ${COMPONENTS}/mod_mock/include
  ${COMPONENTS}/mod_motor/include ${COMPONENTS}/pid/include
  ${COMPONENTS}/mod_imu_bno055/include
  ${COMPONENTS}/core_os/include ${COMPONENTS}/core_os/backend/native/include
  ${COMPONENTS}/diag/include ${COMPONENTS}/diag/priv_include)
target_link_libraries(drivelink_native_core PUBLIC drivelink::protocol Threads::Threads)
target_compile_features(drivelink_native_core PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Match firmware/test/native's warning policy for the shared firmware code.
  target_compile_options(drivelink_native_core PUBLIC -Wno-unused-parameter -Wno-sign-compare)
endif()
add_executable(drivelink-vdev src/main.cpp)
target_link_options(drivelink_native_core INTERFACE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
target_link_libraries(drivelink-vdev PRIVATE drivelink_native_core drivelink_asio drivelink)
set(DRIVELINK_VDEV_BUILD_ID "" CACHE STRING "Override vdev's 16-hex source build id")
set(BUILD_ID ${DRIVELINK_VDEV_BUILD_ID})
if(NOT BUILD_ID)
  execute_process(COMMAND git rev-parse --short=16 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT BUILD_ID MATCHES "^[0-9a-fA-F]+$" OR NOT BUILD_ID)
  # Source archives and Windows worktree gitfiles viewed from WSL have no
  # usable Git metadata. Keep the wire identity defined in either case.
  set(BUILD_ID "0000000000000000")
endif()
target_compile_definitions(drivelink-vdev PRIVATE DRIVELINK_BUILD_ID="${BUILD_ID}")
