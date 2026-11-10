# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# The pure wire library (framing, CRC, scanner, codec) lives outside firmware/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(drivelink_v2)

# CONFIG_DIAG_CONSOLE_LOG=n: compile out every ESP_LOGx in the first-party
# components (components/ and main/), not only the text part of the DIAG_*
# macros. ESP-IDF's own components keep CONFIG_LOG_DEFAULT_LEVEL.
if(NOT CONFIG_DIAG_CONSOLE_LOG)
    set(first_party_components "${CMAKE_SOURCE_DIR}/components")
    set(first_party_main "${CMAKE_SOURCE_DIR}/main")
    idf_build_get_property(build_components BUILD_COMPONENTS)
    foreach(component ${build_components})
        idf_component_get_property(component_dir ${component} COMPONENT_DIR)
        # Not managed_components/: those are third-party.
        cmake_path(IS_PREFIX first_party_components "${component_dir}" NORMALIZE in_components)
        cmake_path(IS_PREFIX first_party_main "${component_dir}" NORMALIZE in_main)
        if(NOT in_components AND NOT in_main)
            continue()
        endif()
        idf_component_get_property(component_lib ${component} COMPONENT_LIB)
        get_target_property(lib_type ${component_lib} TYPE)
        if(NOT lib_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_definitions(${component_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_NONE)
        endif()
    endforeach()
endif()
