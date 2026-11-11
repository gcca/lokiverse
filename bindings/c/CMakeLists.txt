add_library(lokiverse_c STATIC lokiverse.cpp)
target_include_directories(lokiverse_c PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lokiverse_c PRIVATE lokiverse)

set(LOKIVERSE_C_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
set(LOKIVERSE_C_LINK_FILE "${CMAKE_BINARY_DIR}/bindings/lokiverse_c.link" CACHE INTERNAL "")
list(JOIN CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "," lokiverse_c_runtime)

add_executable(lokiverse_c_test tests/lokiverse_c_test.c)
target_link_libraries(lokiverse_c_test PRIVATE lokiverse_c)
set_target_properties(lokiverse_c_test PROPERTIES
  LINKER_LANGUAGE CXX
  C_STANDARD 11
  LINK_DEPENDS "${PROJECT_SOURCE_DIR}/cmake/ExportLinkArgs.cmake"
  RULE_LAUNCH_LINK "\"${CMAKE_COMMAND}\" \"-DOUTPUT=${LOKIVERSE_C_LINK_FILE}\" \"-DRUNTIME=${lokiverse_c_runtime}\" -P \"${PROJECT_SOURCE_DIR}/cmake/ExportLinkArgs.cmake\" --")

if(LOKIVERSE_BUILD_TESTS)
  add_test(NAME lokiverse_c_test COMMAND lokiverse_c_test)
  set_tests_properties(lokiverse_c_test PROPERTIES TIMEOUT 60)
endif()
