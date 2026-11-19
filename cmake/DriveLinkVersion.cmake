# The one DriveLink version (root VERSION file), for every CMake project:
# the firmware and HIL app (PROJECT_VER), protocol/, libdrivelink/ and the host
# superbuild. Include it before project().
#
#   DRIVELINK_VERSION       MAJOR.MINOR.PATCH, for project(VERSION ...)
#   DRIVELINK_VERSION_FULL  the same at a clean checkout of tag v<version>,
#                           else <version>+g<sha7>, ".dirty" when tracked
#                           files changed; the version alone without git;
#                           an environment DRIVELINK_VERSION_FULL wins.
#
# The rules match tools/version.py. Git is read at configure time; VERSION
# changes reconfigure.
include_guard(GLOBAL)

get_filename_component(_drivelink_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(STRINGS "${_drivelink_root}/VERSION" DRIVELINK_VERSION LIMIT_COUNT 1)
string(STRIP "${DRIVELINK_VERSION}" DRIVELINK_VERSION)
if(NOT DRIVELINK_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
  message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, found '${DRIVELINK_VERSION}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_drivelink_root}/VERSION")

set(DRIVELINK_VERSION_FULL "${DRIVELINK_VERSION}")
find_program(DRIVELINK_GIT git)
# An environment DRIVELINK_VERSION_FULL wins (validate.py passes it to WSL
# builds of a Windows worktree, which WSL's git cannot read).
if(NOT "$ENV{DRIVELINK_VERSION_FULL}" STREQUAL "")
  string(REPLACE "." "\\." _drivelink_base_re "${DRIVELINK_VERSION}")
  if(NOT "$ENV{DRIVELINK_VERSION_FULL}" MATCHES "^${_drivelink_base_re}(\\+[0-9A-Za-z.]+)?$")
    message(FATAL_ERROR "DRIVELINK_VERSION_FULL '$ENV{DRIVELINK_VERSION_FULL}' does not extend VERSION ${DRIVELINK_VERSION}")
  endif()
  set(DRIVELINK_VERSION_FULL "$ENV{DRIVELINK_VERSION_FULL}")
elseif(DRIVELINK_GIT)
  execute_process(COMMAND "${DRIVELINK_GIT}" -C "${_drivelink_root}" rev-parse --short=7 HEAD
                  RESULT_VARIABLE _drivelink_rc OUTPUT_VARIABLE _drivelink_sha
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_drivelink_rc EQUAL 0 AND _drivelink_sha)
    execute_process(COMMAND "${DRIVELINK_GIT}" -C "${_drivelink_root}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE _drivelink_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${DRIVELINK_GIT}" -C "${_drivelink_root}" tag --points-at HEAD
                    OUTPUT_VARIABLE _drivelink_tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REPLACE "\n" ";" _drivelink_tags "${_drivelink_tags}")
    if(_drivelink_dirty OR NOT "v${DRIVELINK_VERSION}" IN_LIST _drivelink_tags)
      set(DRIVELINK_VERSION_FULL "${DRIVELINK_VERSION}+g${_drivelink_sha}")
      if(_drivelink_dirty)
        string(APPEND DRIVELINK_VERSION_FULL ".dirty")
      endif()
    endif()
  endif()
endif()
