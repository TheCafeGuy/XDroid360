# Writes xdroid_build_id.h with the current commit, plus "+dirty" when the
# tree has uncommitted changes. Runs on every build (not only at configure
# time), so the boot stamp always matches the sources that were compiled.
# Inputs: ROOT (repository root), OUT (header path).
set(_id "unknown")
find_program(_git git)
if(_git)
  execute_process(COMMAND "${_git}" rev-parse --short HEAD
                  WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE _id
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND "${_git}" --no-optional-locks diff --ignore-submodules=dirty --quiet HEAD --
                  WORKING_DIRECTORY "${ROOT}" RESULT_VARIABLE _dirty
                  ERROR_QUIET)
  if(_dirty)
    string(APPEND _id "+dirty")
  endif()
endif()
set(_text "#define XDROID_BUILD_ID \"${_id}\"\n")
set(_old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _text)
  file(WRITE "${OUT}" "${_text}")
endif()
