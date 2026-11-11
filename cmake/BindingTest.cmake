function(lokiverse_add_binding_test binding tool)
  find_program(LOKIVERSE_${binding}_TOOL NAMES ${tool} REQUIRED)
  add_test(
    NAME lokiverse_${binding}_test
    COMMAND ${CMAKE_COMMAND}
      "-DBINDING=${binding}"
      "-DTOOL=${LOKIVERSE_${binding}_TOOL}"
      "-DLINK_FILE=${LOKIVERSE_C_LINK_FILE}"
      "-DINCLUDE_DIR=${LOKIVERSE_C_INCLUDE_DIR}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_BINARY_DIR}/${binding}"
      -P "${PROJECT_SOURCE_DIR}/cmake/RunBindingTest.cmake")
  set_tests_properties(lokiverse_${binding}_test PROPERTIES TIMEOUT 300)
endfunction()
