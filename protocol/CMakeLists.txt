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
target_include_directories(drivelink_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(drivelink_protocol PROPERTIES EXPORT_NAME protocol)
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
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfigVersion.cmake VERSION 0.1.0 COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/drivelink_protocolConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/drivelink_protocol)
