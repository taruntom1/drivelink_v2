include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(NOT TARGET drivelink_asio)
  FetchContent_Declare(asio
    URL https://codeload.github.com/chriskohlhoff/asio/tar.gz/refs/tags/asio-1-30-2
    URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(asio)
  add_library(drivelink_asio INTERFACE)
  target_include_directories(drivelink_asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
  target_compile_definitions(drivelink_asio INTERFACE ASIO_STANDALONE)
  if(WIN32)
    target_compile_definitions(drivelink_asio INTERFACE _WIN32_WINNT=0x0601 NOMINMAX)
    target_link_libraries(drivelink_asio INTERFACE ws2_32 mswsock)
  endif()
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_Install ON CACHE BOOL "Install JSON dependency" FORCE)
  FetchContent_Declare(json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(json)
elseif(DEFINED nlohmann_json_VERSION AND NOT nlohmann_json_VERSION VERSION_EQUAL "3.11.3")
  message(FATAL_ERROR "libdrivelink's public JSON ABI requires nlohmann JSON 3.11.3")
endif()
