cmake_minimum_required(VERSION 3.16)

# Make production components visible to this standalone test app.
# Using an explicit path keeps this test target independent of component-local test copies.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
# Build only main and what it REQUIRES, not every IDF component.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uart_comm_test)
