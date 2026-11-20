cmake_minimum_required(VERSION 3.24)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/DriveLinkVersion.cmake)
project(drivelink_hardware VERSION ${DRIVELINK_VERSION} LANGUAGES CXX)

# The ros2_control plugin for a DriveLink device behind drivelink-agent
# (README.md). Built with colcon beside drivelink_protocol and drivelink.
find_package(ament_cmake REQUIRED)
find_package(drivelink REQUIRED)
find_package(hardware_interface REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)

set(warnings -Wall -Wextra -Wpedantic -Werror)

# The pure mapping (no ROS types): parameters, labels to refs, groups, the stale rule.
add_library(drivelink_hardware_mapping STATIC src/mapping.cpp)
target_compile_features(drivelink_hardware_mapping PUBLIC cxx_std_23)
target_include_directories(drivelink_hardware_mapping PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(drivelink_hardware_mapping PUBLIC drivelink::drivelink)
target_compile_options(drivelink_hardware_mapping PRIVATE ${warnings})
set_target_properties(drivelink_hardware_mapping PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(drivelink_hardware SHARED src/system.cpp)
target_compile_features(drivelink_hardware PUBLIC cxx_std_23)
target_include_directories(drivelink_hardware PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include/${PROJECT_NAME}>)
target_link_libraries(drivelink_hardware PRIVATE drivelink_hardware_mapping)
target_link_libraries(drivelink_hardware PUBLIC
  hardware_interface::hardware_interface pluginlib::pluginlib rclcpp::rclcpp rclcpp_lifecycle::rclcpp_lifecycle)
target_compile_options(drivelink_hardware PRIVATE ${warnings})
pluginlib_export_plugin_description_file(hardware_interface drivelink_hardware.xml)

install(TARGETS drivelink_hardware LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_mapping test/test_mapping.cpp)
  target_link_libraries(test_mapping drivelink_hardware_mapping)
  target_compile_options(test_mapping PRIVATE ${warnings})

  # Against drivelink-vdev behind drivelink-agent. tools/ros_check.sh turns it
  # on through the environment and names the programs from the host superbuild.
  set(integration OFF)
  if(DEFINED ENV{DRIVELINK_HW_INTEGRATION})
    set(integration $ENV{DRIVELINK_HW_INTEGRATION})
  endif()
  option(DRIVELINK_HW_INTEGRATION "Build the plugin's integration test against vdev and the agent" ${integration})
  if(DRIVELINK_HW_INTEGRATION)
    foreach(program DRIVELINK_VDEV DRIVELINK_AGENT DRIVELINK_CLI)
      if(NOT DEFINED ${program} AND DEFINED ENV{${program}})
        set(${program} $ENV{${program}})
      endif()
      if(NOT EXISTS "${${program}}")
        message(FATAL_ERROR "DRIVELINK_HW_INTEGRATION needs ${program} (a built program), got '${${program}}'")
      endif()
    endforeach()
    ament_add_gtest(test_integration test/test_integration.cpp test/harness.cpp TIMEOUT 300
      ENV DRIVELINK_VDEV=${DRIVELINK_VDEV} DRIVELINK_AGENT=${DRIVELINK_AGENT} DRIVELINK_CLI=${DRIVELINK_CLI})
    target_link_libraries(test_integration drivelink_hardware_mapping hardware_interface::hardware_interface
      rclcpp::rclcpp)
    target_compile_options(test_integration PRIVATE ${warnings})
    # The test loads the plugin through pluginlib from the install space.
    add_dependencies(test_integration drivelink_hardware)
  endif()
endif()

ament_export_include_directories(include/${PROJECT_NAME})
ament_export_libraries(drivelink_hardware)
ament_export_dependencies(drivelink hardware_interface pluginlib rclcpp rclcpp_lifecycle)
ament_package()
