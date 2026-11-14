include(../cmake/Json.cmake)
add_executable(drivelink-cli src/main.cpp src/format.cpp)
target_link_libraries(drivelink-cli PRIVATE drivelink nlohmann_json::nlohmann_json)
