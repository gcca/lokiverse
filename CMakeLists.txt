cmake_minimum_required(VERSION 4.3)

project(lokiverse LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER
    ${CMAKE_COMMAND} -E env
    CCACHE_SLOPPINESS=pch_defines,time_macros
    "CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}"
    "${CCACHE_PROGRAM}")
endif()

option(LOKIVERSE_BUILD_TESTS "Build lokiverse unit tests" ON)
option(LOKIVERSE_BUILD_PYTHON "Build the CPython extension" OFF)
option(LOKIVERSE_BUILD_C "Build the C API static library" OFF)
option(LOKIVERSE_BUILD_GO "Test the Go binding" OFF)
option(LOKIVERSE_BUILD_RUST "Test the Rust binding" OFF)
option(LOKIVERSE_BUILD_ZIG "Test the Zig binding" OFF)
if(LOKIVERSE_BUILD_GO OR LOKIVERSE_BUILD_RUST OR LOKIVERSE_BUILD_ZIG)
  set(LOKIVERSE_BUILD_C ON)
endif()
option(LOKIVERSE_INSTALL "Install lokiverse and its CMake package" OFF)
option(LOKIVERSE_STATIC_DEPS "Link system Protobuf, Abseil and Snappy static archives" OFF)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(cmake/Dependencies.cmake)

set(lokiverse_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${lokiverse_generated_dir}/push.pb.cc" "${lokiverse_generated_dir}/push.pb.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${lokiverse_generated_dir}"
  COMMAND protobuf::protoc
    "--cpp_out=${lokiverse_generated_dir}"
    "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto"
    "--proto_path=${LOKIVERSE_PROTOBUF_PROTO_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/push.proto"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/push.proto" protobuf::protoc
  VERBATIM)

add_library(lokiverse STATIC src/lokiverse.cpp "${lokiverse_generated_dir}/push.pb.cc")
target_include_directories(lokiverse PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(lokiverse SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
target_include_directories(lokiverse PRIVATE "${lokiverse_generated_dir}")
target_link_libraries(lokiverse PUBLIC Threads::Threads PRIVATE protobuf::libprotobuf Snappy::snappy)

if(LOKIVERSE_BUILD_PYTHON)
  set_target_properties(lokiverse PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(LOKIVERSE_BUILD_TESTS)
  enable_testing()
  include(GoogleTest)

  add_executable(lokiverse_test tests/lokiverse_test.cc)
  target_include_directories(lokiverse_test PRIVATE "${lokiverse_generated_dir}")
  target_include_directories(lokiverse_test SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rd")
  target_link_libraries(lokiverse_test PRIVATE lokiverse GTest::gmock GTest::gtest_main
    protobuf::libprotobuf Snappy::snappy)
  gtest_discover_tests(lokiverse_test)
endif()

if(LOKIVERSE_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()

if(LOKIVERSE_BUILD_C)
  add_subdirectory(bindings/c)
endif()
foreach(lokiverse_binding IN ITEMS GO RUST ZIG)
  if(LOKIVERSE_BUILD_${lokiverse_binding} AND LOKIVERSE_BUILD_TESTS)
    string(TOLOWER "${lokiverse_binding}" lokiverse_binding_dir)
    add_subdirectory("bindings/${lokiverse_binding_dir}")
  endif()
endforeach()

if(LOKIVERSE_INSTALL)
  include(CMakePackageConfigHelpers)
  set(lokiverse_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/lokiverse")
  install(TARGETS lokiverse EXPORT lokiverseTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES src/lokiverse.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(EXPORT lokiverseTargets NAMESPACE lokiverse:: DESTINATION "${lokiverse_cmake_dir}")
  configure_package_config_file(cmake/lokiverseConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/lokiverseConfig.cmake"
    INSTALL_DESTINATION "${lokiverse_cmake_dir}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lokiverseConfig.cmake"
    cmake/lokiverseStaticDeps.cmake DESTINATION "${lokiverse_cmake_dir}")
endif()
