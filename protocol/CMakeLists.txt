cmake_minimum_required(VERSION 3.16)

# Pure C++20 wire library: framing, CRC, frame scanner and the field codec.
# No ESP-IDF, FreeRTOS or OS dependency.
#
#   - Under ESP-IDF this directory is a component, added to the firmware
#     through EXTRA_COMPONENT_DIRS.
#   - Otherwise it is an ordinary static library, drivelink::protocol, for the
#     native tests (and later the host software).

set(DRIVELINK_PROTOCOL_SOURCES
    src/crc.cpp
    src/framing.cpp
    src/scanner.cpp
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${DRIVELINK_PROTOCOL_SOURCES}
        INCLUDE_DIRS "include"
    )
    return()
endif()

project(drivelink_protocol LANGUAGES CXX)

add_library(drivelink_protocol STATIC ${DRIVELINK_PROTOCOL_SOURCES})
add_library(drivelink::protocol ALIAS drivelink_protocol)
target_include_directories(drivelink_protocol PUBLIC include)
target_compile_features(drivelink_protocol PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(drivelink_protocol PRIVATE -Wall -Wextra)
endif()
