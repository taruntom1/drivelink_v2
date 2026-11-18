cmake_minimum_required(VERSION 3.16)

# Make production components visible to this standalone test app.
# Using an explicit path keeps this test target independent of component-local test copies.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components"
                         "${CMAKE_CURRENT_LIST_DIR}/../../../protocol")
# Build only main and what it REQUIRES, not every IDF component.
set(COMPONENTS main)

# The app version (esp_app_desc_t.version, GET_INFO's firmware_version) comes
# from the root VERSION file; bench_fw.py's -D PROJECT_VER override wins.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/DriveLinkVersion.cmake)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "${DRIVELINK_VERSION_FULL}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uart_comm_test)
