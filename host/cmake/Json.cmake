include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_Install ON CACHE BOOL "Install JSON dependency" FORCE)
  FetchContent_Declare(json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(json)
elseif(DEFINED nlohmann_json_VERSION AND NOT nlohmann_json_VERSION VERSION_EQUAL "3.11.3")
  message(FATAL_ERROR "CLI formatting requires nlohmann JSON 3.11.3")
endif()
