if(NOT DEFINED SOURCE_DIR OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "SOURCE_DIR and TEST_ROOT are required")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

set(disabled_dir "${TEST_ROOT}/python-disabled")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${disabled_dir}"
          -DBUILD_TESTING=ON -DAXISSIM_BUILD_GUI=OFF
          -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE
  RESULT_VARIABLE disabled_result OUTPUT_VARIABLE disabled_stdout ERROR_VARIABLE disabled_stderr)
if(NOT disabled_result EQUAL 0)
  message(FATAL_ERROR "Python-disabled configure failed: ${disabled_stdout}${disabled_stderr}")
endif()
string(CONCAT disabled_log "${disabled_stdout}" "${disabled_stderr}")
string(REGEX REPLACE "[\r\n ]+" " " disabled_log "${disabled_log}")
if(NOT disabled_log MATCHES "Python-backed tests are not registered")
  message(FATAL_ERROR "Python-disabled configure did not disclose reduced test registration")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${disabled_dir}" -N
                RESULT_VARIABLE disabled_list_result OUTPUT_VARIABLE disabled_tests ERROR_VARIABLE disabled_test_error)
if(NOT disabled_list_result EQUAL 0 OR disabled_tests MATCHES "run_provenance_cli_contract")
  message(FATAL_ERROR "Python-disabled configuration registered Python study tests: ${disabled_tests}${disabled_test_error}")
endif()

if(DEFINED PYTHON_EXECUTABLE AND NOT PYTHON_EXECUTABLE STREQUAL "")
  set(enabled_dir "${TEST_ROOT}/python-enabled")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${enabled_dir}"
            -DBUILD_TESTING=ON -DAXISSIM_BUILD_GUI=OFF
            "-DPython3_EXECUTABLE=${PYTHON_EXECUTABLE}"
    RESULT_VARIABLE enabled_result OUTPUT_VARIABLE enabled_stdout ERROR_VARIABLE enabled_stderr)
  if(NOT enabled_result EQUAL 0)
    message(FATAL_ERROR "Python-enabled configure failed: ${enabled_stdout}${enabled_stderr}")
  endif()
  string(CONCAT enabled_log "${enabled_stdout}" "${enabled_stderr}")
  string(REGEX REPLACE "[\r\n ]+" " " enabled_log "${enabled_log}")
  if(NOT enabled_log MATCHES "enables the full Python 3.10")
    message(FATAL_ERROR "Python-enabled configure did not confirm full suite registration: ${enabled_log}")
  endif()
  execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${enabled_dir}" -N
                  RESULT_VARIABLE enabled_list_result OUTPUT_VARIABLE enabled_tests ERROR_VARIABLE enabled_test_error)
  if(NOT enabled_list_result EQUAL 0 OR NOT enabled_tests MATCHES "run_provenance_cli_contract")
    message(FATAL_ERROR "Python-enabled configuration omitted provenance test registration: ${enabled_tests}${enabled_test_error}")
  endif()
else()
  message(STATUS "Python-available registration path not exercised because no Python 3.10+ interpreter was passed")
endif()
message(STATUS "CMake Python test-registration paths validated")
