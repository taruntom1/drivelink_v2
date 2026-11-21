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
    src/sha256.cpp
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${DRIVELINK_PROTOCOL_SOURCES}
        INCLUDE_DIRS "include"
    )
    return()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/DriveLinkVersion.cmake)
project(drivelink_protocol VERSION ${DRIVELINK_VERSION} LANGUAGES CXX)

add_library(drivelink_protocol STATIC ${DRIVELINK_PROTOCOL_SOURCES})
add_library(drivelink::protocol ALIAS drivelink_protocol)
target_include_directories(drivelink_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
# POSITION_INDEPENDENT_CODE: the ROS plugin links this static library into a
# shared object (host/ros/drivelink_hardware).
set_target_properties(drivelink_protocol PROPERTIES EXPORT_NAME protocol POSITION_INDEPENDENT_CODE ON)
target_compile_features(drivelink_protocol PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(drivelink_protocol PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS drivelink_protocol EXPORT drivelinkProtocolTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT drivelinkProtocolTargets NAMESPACE drivelink:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/drivelink_protocol)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfig.cmake "include(\"\${CMAKE_CURRENT_LIST_DIR}/drivelinkProtocolTargets.cmake\")\n")
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfigVersion.cmake VERSION ${DRIVELINK_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/drivelink_protocol)
