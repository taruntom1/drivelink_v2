cmake_minimum_required(VERSION 3.24)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/DriveLinkVersion.cmake)
project(drivelink_bringup VERSION ${DRIVELINK_VERSION} LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(controller_manager_msgs REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(drivelink REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)

set(warnings -Wall -Wextra -Wpedantic -Werror)
add_executable(diagnostics src/diagnostics.cpp)
target_compile_features(diagnostics PRIVATE cxx_std_23)
target_compile_options(diagnostics PRIVATE ${warnings})
target_link_libraries(diagnostics PRIVATE drivelink::drivelink rclcpp::rclcpp
  ${controller_manager_msgs_TARGETS} ${diagnostic_msgs_TARGETS} ${lifecycle_msgs_TARGETS} ${std_srvs_TARGETS})
install(TARGETS diagnostics DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config launch urdf DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_recovery test/test_recovery.cpp)
  target_compile_features(test_recovery PRIVATE cxx_std_23)
  target_compile_options(test_recovery PRIVATE ${warnings})

  if("$ENV{DRIVELINK_HW_INTEGRATION}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    ament_add_test(test_bringup TIMEOUT 240 GENERATE_RESULT_FOR_RETURN_CODE_ZERO
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/../../test/ros_bringup_check.py --mock)
  endif()
endif()

ament_package()
