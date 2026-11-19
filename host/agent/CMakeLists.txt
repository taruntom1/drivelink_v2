# drivelink-agent (H3a): Linux only; owns device ports and shares them over TCP.
# Built by the superbuild on Linux. Not a colcon package (COLCON_IGNORE).
if(WIN32)
  message(FATAL_ERROR "drivelink-agent is Linux-only")
endif()
add_library(drivelink_agent_core STATIC src/config.cpp src/groups.cpp src/agent.cpp)
target_include_directories(drivelink_agent_core PUBLIC src)
target_link_libraries(drivelink_agent_core PUBLIC drivelink PRIVATE drivelink_asio nlohmann_json::nlohmann_json Threads::Threads)
add_executable(drivelink-agent src/main.cpp)
target_link_libraries(drivelink-agent PRIVATE drivelink_agent_core)
target_compile_definitions(drivelink-agent PRIVATE DRIVELINK_AGENT_VERSION="${DRIVELINK_VERSION_FULL}")
install(TARGETS drivelink-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
  add_executable(agent_tests test/test_main.cpp test/test_config.cpp test/test_groups.cpp)
  target_include_directories(agent_tests SYSTEM PRIVATE ../../firmware/test/native/third_party/doctest)
  target_link_libraries(agent_tests PRIVATE drivelink_agent_core)
  add_test(NAME agent-unit COMMAND agent_tests)
  set_tests_properties(agent-unit PROPERTIES TIMEOUT 30 LABELS agent)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME agent-integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/agent_integration.py
    --agent $<TARGET_FILE:drivelink-agent> --cli $<TARGET_FILE:drivelink-cli> --vdev $<TARGET_FILE:drivelink-vdev>)
  set_tests_properties(agent-integration PROPERTIES TIMEOUT 240 LABELS agent)
endif()
