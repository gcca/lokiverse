cmake_minimum_required(VERSION 4.3)

if(NOT EXISTS "${LINK_FILE}")
  message(FATAL_ERROR "lokiverse: ${LINK_FILE} is missing; build the lokiverse_c_test target first")
endif()
file(STRINGS "${LINK_FILE}" arguments)

if(BINDING STREQUAL "go")
  set(flags "")
  foreach(argument IN LISTS arguments)
    if(argument MATCHES "[ \t\"']")
      string(REPLACE "\"" "\\\"" argument "${argument}")
      set(argument "\"${argument}\"")
    endif()
    list(APPEND flags "${argument}")
  endforeach()
  list(JOIN flags " " flags)
  set(command ${CMAKE_COMMAND} -E env
    "CGO_ENABLED=1"
    "CGO_CFLAGS=-I${INCLUDE_DIR}"
    "CGO_LDFLAGS=${flags}"
    "GOCACHE=${WORK_DIR}/cache"
    "GOFLAGS=-mod=mod"
    "GOWORK=off"
    "${TOOL}" test -count=1 ./...)
elseif(BINDING STREQUAL "rust")
  set(command ${CMAKE_COMMAND} -E env
    "LOKIVERSE_LINK_ARGS=${LINK_FILE}"
    "${TOOL}" test --offline --manifest-path "${SOURCE_DIR}/Cargo.toml" --target-dir "${WORK_DIR}")
elseif(BINDING STREQUAL "zig")
  set(runtime "")
  if(CMAKE_HOST_APPLE)
    execute_process(COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(EXISTS "${sdk}/usr/lib/libc++.tbd")
      set(runtime "${sdk}/usr/lib/libc++.tbd")
    endif()
  endif()
  set(options "")
  foreach(argument IN LISTS arguments)
    if(runtime AND argument STREQUAL "-lc++")
      set(argument "${runtime}")
    endif()
    list(APPEND options "-Dlokiverse-link=${argument}")
  endforeach()
  set(command "${TOOL}" build test --summary all
    --cache-dir "${WORK_DIR}/cache" --prefix "${WORK_DIR}/out" ${options})
else()
  message(FATAL_ERROR "lokiverse: unknown binding ${BINDING}")
endif()

execute_process(COMMAND ${command} WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "lokiverse: ${BINDING} binding tests failed")
endif()
