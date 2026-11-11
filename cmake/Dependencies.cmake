include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/lokiverseStaticDeps.cmake")

if(LOKIVERSE_STATIC_DEPS)
  lokiverse_find_static_absl()
endif()

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/probe-protobuf"
  "${CMAKE_CURRENT_BINARY_DIR}/probe-protobuf")

if(LOKIVERSE_SYSTEM_PROTOBUF_FOUND)
  message(STATUS "lokiverse: using system Protobuf and protoc")
  get_target_property(protobuf_include_dirs protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
  unset(LOKIVERSE_PROTOBUF_PROTO_PATH CACHE)
  find_path(LOKIVERSE_PROTOBUF_PROTO_PATH google/protobuf/timestamp.proto
    HINTS ${protobuf_include_dirs} REQUIRED NO_DEFAULT_PATH NO_CACHE)
else()
  if(LOKIVERSE_INSTALL OR LOKIVERSE_STATIC_DEPS)
    message(FATAL_ERROR "lokiverse: LOKIVERSE_INSTALL and LOKIVERSE_STATIC_DEPS require a compatible system Protobuf")
  endif()
  message(STATUS "lokiverse: fetching Protobuf v36.2")
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
  set(protobuf_FORCE_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(protobuf
    URL https://codeload.github.com/protocolbuffers/protobuf/tar.gz/refs/tags/v36.2
    URL_HASH SHA256=7e7eaf580ec77f1a392ff1ab1013c13d3b04a6716f002f697f67f3c1113f0530
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(protobuf)
  set(LOKIVERSE_PROTOBUF_PROTO_PATH "${protobuf_SOURCE_DIR}/src")
endif()

find_package(Snappy QUIET CONFIG)
set(lokiverse_snappy_config_found FALSE)
if(TARGET Snappy::snappy)
  set(lokiverse_snappy_config_found TRUE)
endif()
if(NOT TARGET Snappy::snappy AND NOT CMAKE_DISABLE_FIND_PACKAGE_Snappy)
  find_path(lokiverse_snappy_include_dir snappy.h NO_CACHE)
  find_library(lokiverse_snappy_library NAMES snappy NO_CACHE)
  if(EXISTS "${lokiverse_snappy_include_dir}/snappy.h" AND EXISTS "${lokiverse_snappy_library}")
    add_library(Snappy::snappy UNKNOWN IMPORTED)
    set_target_properties(Snappy::snappy PROPERTIES
      IMPORTED_LOCATION "${lokiverse_snappy_library}"
      INTERFACE_INCLUDE_DIRECTORIES "${lokiverse_snappy_include_dir}")
  endif()
endif()
if(LOKIVERSE_INSTALL AND NOT lokiverse_snappy_config_found)
  message(FATAL_ERROR "lokiverse: LOKIVERSE_INSTALL requires a system Snappy CMake package")
endif()
if(TARGET Snappy::snappy)
  message(STATUS "lokiverse: using system Snappy")
else()
  if(LOKIVERSE_STATIC_DEPS)
    message(FATAL_ERROR "lokiverse: LOKIVERSE_STATIC_DEPS requires a system Snappy")
  endif()
  message(STATUS "lokiverse: fetching Snappy 1.3.1")
  set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  FetchContent_Declare(snappy
    URL https://codeload.github.com/google/snappy/tar.gz/refs/tags/1.3.1
    URL_HASH SHA256=893f708a0bf4b5529d555ffcee390e940e932fcf90261f682604475a76cd0247
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(snappy)
  if(NOT TARGET Snappy::snappy)
    add_library(Snappy::snappy ALIAS snappy)
  endif()
endif()

if(LOKIVERSE_STATIC_DEPS)
  lokiverse_use_static_archives(protobuf::libprotobuf protobuf::libprotobuf-lite
    utf8_range::utf8_validity utf8_range::utf8_range Snappy::snappy)
endif()

if(LOKIVERSE_BUILD_TESTS)
  find_package(GTest QUIET CONFIG)
  if(TARGET GTest::gmock AND TARGET GTest::gtest_main)
    message(STATUS "lokiverse: using system GoogleTest ${GTest_VERSION}")
  else()
    message(STATUS "lokiverse: fetching GoogleTest 1.18.0")
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      URL https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.18.0
      URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
      EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
