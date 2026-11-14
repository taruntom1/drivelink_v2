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
