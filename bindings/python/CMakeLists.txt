find_package(Python 3.13 REQUIRED COMPONENTS Interpreter Development.Module)

Python_add_library(lokiverse_python MODULE WITH_SOABI module.cpp)
set_target_properties(lokiverse_python PROPERTIES OUTPUT_NAME lokiverse)
target_link_libraries(lokiverse_python PRIVATE lokiverse)

install(TARGETS lokiverse_python LIBRARY DESTINATION . RUNTIME DESTINATION .)

if(LOKIVERSE_BUILD_TESTS)
  set(lokiverse_python_test_paths "$<TARGET_FILE_DIR:lokiverse_python>" "${Python_SITELIB}" "${Python_SITEARCH}")
  list(REMOVE_DUPLICATES lokiverse_python_test_paths)
  cmake_path(CONVERT "${lokiverse_python_test_paths}" TO_NATIVE_PATH_LIST lokiverse_python_test_path)
  add_test(
    NAME lokiverse_python_test
    COMMAND ${CMAKE_COMMAND} -E env
      "PYTHONPATH=${lokiverse_python_test_path}"
      "${Python_EXECUTABLE}" -S -X dev -m unittest discover
      -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -v)
  set_tests_properties(lokiverse_python_test PROPERTIES TIMEOUT 60)
endif()
