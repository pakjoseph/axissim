cmake_minimum_required(VERSION 3.20)
project(axissim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(AXISSIM_BUILD_GUI "Build the Qt 6 desktop explorer when Qt Widgets is available" ON)
message(STATUS "AxisSim requires CMake 3.20+ and a compiler with C++20 support; using ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

set(AXISSIM_RELEASE_TAG "" CACHE STRING "Optional Git tag identity for a release-labeled package; must resolve to this exact source revision")
set(AXISSIM_PACKAGE_RELEASE_TAG "unknown")

set(AXISSIM_SOURCE_REVISION "unknown")
set(AXISSIM_SOURCE_TREE_STATE "unknown")
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse --verify HEAD
                  RESULT_VARIABLE AXISSIM_GIT_RESULT OUTPUT_VARIABLE AXISSIM_GIT_HEAD
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(AXISSIM_GIT_RESULT EQUAL 0 AND AXISSIM_GIT_HEAD MATCHES "^[0-9a-fA-F]+$")
    string(TOLOWER "${AXISSIM_GIT_HEAD}" AXISSIM_SOURCE_REVISION)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} diff-index --quiet HEAD --
                    RESULT_VARIABLE AXISSIM_GIT_DIRTY_RESULT ERROR_QUIET)
    if(AXISSIM_GIT_DIRTY_RESULT EQUAL 0)
      set(AXISSIM_SOURCE_TREE_STATE "clean")
    else()
      set(AXISSIM_SOURCE_TREE_STATE "modified")
    endif()
    # Regenerate the embedded build identity when any tracked source input
    # changes after the initial configure. Package scripts also reconfigure
    # immediately before building, so package identities cannot stay stale.
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} ls-files
                    RESULT_VARIABLE AXISSIM_TRACKED_FILES_RESULT
                    OUTPUT_VARIABLE AXISSIM_TRACKED_FILES
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(AXISSIM_TRACKED_FILES_RESULT EQUAL 0)
      string(REPLACE "\n" ";" AXISSIM_TRACKED_FILE_LIST "${AXISSIM_TRACKED_FILES}")
      set(AXISSIM_TRACKED_CONFIGURE_DEPENDS)
      foreach(AXISSIM_TRACKED_FILE IN LISTS AXISSIM_TRACKED_FILE_LIST)
        if(NOT AXISSIM_TRACKED_FILE STREQUAL "")
          list(APPEND AXISSIM_TRACKED_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${AXISSIM_TRACKED_FILE}")
        endif()
      endforeach()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${AXISSIM_TRACKED_CONFIGURE_DEPENDS})
    endif()
  endif()
endif()

if(AXISSIM_RELEASE_TAG)
  if(NOT Git_FOUND OR AXISSIM_SOURCE_REVISION STREQUAL "unknown")
    message(FATAL_ERROR "A release tag cannot be verified without Git source identity")
  endif()
  if(AXISSIM_SOURCE_TREE_STATE STREQUAL "modified")
    message(FATAL_ERROR "Release-labeled packages require a clean tracked source tree")
  endif()
  # Keep values inserted into the generated JSON and C++ metadata safely
  # representable. Git's ref validator below remains authoritative for tag
  # structure; this whitelist additionally prevents quote/backslash injection.
  set(AXISSIM_SAFE_TAG_CHARACTERS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._/-")
  string(LENGTH "${AXISSIM_RELEASE_TAG}" AXISSIM_RELEASE_TAG_LENGTH)
  math(EXPR AXISSIM_RELEASE_TAG_LAST_INDEX "${AXISSIM_RELEASE_TAG_LENGTH} - 1")
  foreach(AXISSIM_TAG_INDEX RANGE 0 ${AXISSIM_RELEASE_TAG_LAST_INDEX})
    string(SUBSTRING "${AXISSIM_RELEASE_TAG}" ${AXISSIM_TAG_INDEX} 1 AXISSIM_TAG_CHARACTER)
    string(FIND "${AXISSIM_SAFE_TAG_CHARACTERS}" "${AXISSIM_TAG_CHARACTER}" AXISSIM_TAG_CHARACTER_INDEX)
    if(AXISSIM_TAG_CHARACTER_INDEX EQUAL -1)
      message(FATAL_ERROR "Release tag identity contains unsupported characters")
    endif()
  endforeach()
  string(FIND "${AXISSIM_RELEASE_TAG}" ".." AXISSIM_RELEASE_TAG_DOTDOT_INDEX)
  if(NOT AXISSIM_RELEASE_TAG_DOTDOT_INDEX EQUAL -1)
    message(FATAL_ERROR "Release tag identity contains unsupported characters: ${AXISSIM_RELEASE_TAG}")
  endif()
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} check-ref-format "refs/tags/${AXISSIM_RELEASE_TAG}"
                  RESULT_VARIABLE AXISSIM_TAG_FORMAT_RESULT ERROR_QUIET)
  if(NOT AXISSIM_TAG_FORMAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Invalid release tag name: ${AXISSIM_RELEASE_TAG}")
  endif()
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse "${AXISSIM_RELEASE_TAG}^{commit}"
                  RESULT_VARIABLE AXISSIM_TAG_RESULT OUTPUT_VARIABLE AXISSIM_TAG_COMMIT
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  string(TOLOWER "${AXISSIM_TAG_COMMIT}" AXISSIM_TAG_COMMIT)
  if(NOT AXISSIM_TAG_RESULT EQUAL 0 OR NOT AXISSIM_TAG_COMMIT STREQUAL AXISSIM_SOURCE_REVISION)
    message(FATAL_ERROR "Release tag ${AXISSIM_RELEASE_TAG} does not resolve to packaged source revision ${AXISSIM_SOURCE_REVISION}")
  endif()
  set(AXISSIM_PACKAGE_KIND "release")
  set(AXISSIM_PACKAGE_ID "${AXISSIM_RELEASE_TAG}")
  set(AXISSIM_PACKAGE_RELEASE_TAG "${AXISSIM_RELEASE_TAG}")
else()
  set(AXISSIM_PACKAGE_KIND "validation")
  string(LENGTH "${AXISSIM_SOURCE_REVISION}" AXISSIM_SOURCE_REVISION_LENGTH)
  if(AXISSIM_SOURCE_REVISION_LENGTH GREATER_EQUAL 12)
    string(SUBSTRING "${AXISSIM_SOURCE_REVISION}" 0 12 AXISSIM_SHORT_REVISION)
  else()
    set(AXISSIM_SHORT_REVISION "unknown")
  endif()
  set(AXISSIM_PACKAGE_ID "validation-${AXISSIM_SHORT_REVISION}")
  if(AXISSIM_SOURCE_TREE_STATE STREQUAL "modified")
    string(APPEND AXISSIM_PACKAGE_ID "-modified")
  endif()
endif()

set(AXISSIM_QT_VERSION "not-applicable")
set(AXISSIM_QT_RUNTIME_MODE "not-bundled")
set(AXISSIM_QT_SOURCES "")
if(AXISSIM_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6Widgets_FOUND)
    set(AXISSIM_QT_VERSION "${Qt6_VERSION}")
    if(APPLE OR WIN32)
      set(AXISSIM_QT_RUNTIME_MODE "bundled-dynamic")
    else()
      set(AXISSIM_QT_RUNTIME_MODE "system-dynamic")
    endif()
    get_filename_component(AXISSIM_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
    file(GLOB AXISSIM_QT_SOURCES
         "${AXISSIM_QT_PREFIX}/sbom/*.spdx.json"
         "${AXISSIM_QT_PREFIX}/share/qt/sbom/*.spdx"
         "${AXISSIM_QT_PREFIX}/share/qt/sbom/*.spdx.json")
  endif()
endif()

set(AXISSIM_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(AXISSIM_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(AXISSIM_TARGET_PLATFORM "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "unspecified")
endif()
set(AXISSIM_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
set(AXISSIM_PACKAGE_SBOM_STATUS "not-applicable")
if(NOT AXISSIM_QT_RUNTIME_MODE STREQUAL "not-bundled")
  if(AXISSIM_QT_SOURCES)
    set(AXISSIM_PACKAGE_SBOM_STATUS "included")
  else()
    set(AXISSIM_PACKAGE_SBOM_STATUS "unavailable")
    if(AXISSIM_QT_RUNTIME_MODE STREQUAL "bundled-dynamic")
      message(WARNING "Qt is dynamically bundled but its installation SBOM was not found; package notices will identify this limitation")
    else()
      message(STATUS "Qt installation SBOM was not found; system Qt packages remain host-managed")
    endif()
  endif()
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated" "${CMAKE_BINARY_DIR}/package-assets/licenses" "${CMAKE_BINARY_DIR}/package-assets/third-party")
configure_file(src/build_info.hpp.in "${CMAKE_BINARY_DIR}/generated/build_info.hpp" @ONLY)
configure_file(resources/package-provenance.json.in "${CMAKE_BINARY_DIR}/package-assets/package-provenance.json" @ONLY)
configure_file(LICENSE "${CMAKE_BINARY_DIR}/package-assets/LICENSE" COPYONLY)
configure_file(docs/THIRD_PARTY_NOTICES.md "${CMAKE_BINARY_DIR}/package-assets/THIRD_PARTY_NOTICES.md" COPYONLY)
configure_file(docs/licenses/LGPL-3.0.txt "${CMAKE_BINARY_DIR}/package-assets/licenses/LGPL-3.0.txt" COPYONLY)
file(WRITE "${CMAKE_BINARY_DIR}/package-assets/third-party/qt-sbom-status.txt"
     "qt_installation_sbom_status=${AXISSIM_PACKAGE_SBOM_STATUS}\n")
if(AXISSIM_QT_SOURCES)
  list(GET AXISSIM_QT_SOURCES 0 AXISSIM_QT_SBOM)
  configure_file("${AXISSIM_QT_SBOM}" "${CMAKE_BINARY_DIR}/package-assets/third-party/qt-installation-sbom.spdx" COPYONLY)
endif()

function(axissim_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

# Keep configuration/profile behavior in a reusable library. The command-line
# tool and direct model tests consume the same parsing and validation path.
add_library(axissim-core STATIC
  src/config.cpp
  src/gemm_json.cpp
  src/gemm_mapping.cpp
  src/gemm_study_contract.cpp
  src/matrix_architecture.cpp
  src/matrix_execution.cpp
  src/metrics.cpp
  src/profile.cpp
  src/run_provenance.cpp
  src/sha256.cpp
  src/study_contract.cpp)
target_include_directories(axissim-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
axissim_enable_strict_warnings(axissim-core)

add_executable(axissim src/cli/main.cpp)
target_link_libraries(axissim PRIVATE axissim-core)
target_include_directories(axissim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
axissim_enable_strict_warnings(axissim)

if(AXISSIM_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6Widgets_FOUND)
    set(CMAKE_AUTORCC ON)
    set(AXISSIM_GUI_SOURCES
        src/gui/main.cpp
        src/gui/charts.cpp
        src/gui/charts.hpp
        src/gui/theme.cpp
        src/gui/theme.hpp
        resources/gui.qrc
        resources/AxisSim.icns)
    if(APPLE)
      set_source_files_properties(resources/AxisSim.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
      add_executable(axissim-gui MACOSX_BUNDLE ${AXISSIM_GUI_SOURCES})
      add_dependencies(axissim-gui axissim)
      set_target_properties(axissim-gui PROPERTIES
                            OUTPUT_NAME "AxisSim"
                            MACOSX_BUNDLE_BUNDLE_NAME "AxisSim"
                            MACOSX_BUNDLE_ICON_FILE "AxisSim.icns"
                            MACOSX_BUNDLE_GUI_IDENTIFIER "dev.axissim.explorer"
                            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.2"
                            MACOSX_BUNDLE_BUNDLE_VERSION "0.2")
      add_custom_command(TARGET axissim-gui POST_BUILD
                         COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                 $<TARGET_FILE:axissim>
                                 $<TARGET_BUNDLE_DIR:axissim-gui>/Contents/MacOS/axissim-cli
                         COMMAND ${CMAKE_COMMAND} -E make_directory
                                 $<TARGET_BUNDLE_DIR:axissim-gui>/Contents/Resources/axissim-data
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${CMAKE_SOURCE_DIR}/configs
                                 $<TARGET_BUNDLE_DIR:axissim-gui>/Contents/Resources/axissim-data/configs
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${CMAKE_SOURCE_DIR}/reference_data
                                 $<TARGET_BUNDLE_DIR:axissim-gui>/Contents/Resources/axissim-data/reference_data
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${CMAKE_BINARY_DIR}/package-assets
                                 $<TARGET_BUNDLE_DIR:axissim-gui>/Contents/Resources/axissim-package
                         COMMENT "Bundling AxisSim CLI, scenarios, and reference data")
    else()
      add_executable(axissim-gui ${AXISSIM_GUI_SOURCES})
      # A portable Windows/Linux layout keeps the GUI, authoritative CLI, and
      # checked-in scenarios together. The GUI resolves `axissim-data` beside
      # its executable before falling back to a developer source tree.
      add_dependencies(axissim-gui axissim)
      set(AXISSIM_PORTABLE_DIR "${CMAKE_BINARY_DIR}/portable")
      add_custom_command(TARGET axissim-gui POST_BUILD
                         COMMAND ${CMAKE_COMMAND} -E make_directory ${AXISSIM_PORTABLE_DIR}/axissim-data
                         COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                 $<TARGET_FILE:axissim>
                                 ${AXISSIM_PORTABLE_DIR}/axissim-cli${CMAKE_EXECUTABLE_SUFFIX}
                         COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                 $<TARGET_FILE:axissim-gui>
                                 ${AXISSIM_PORTABLE_DIR}/AxisSim${CMAKE_EXECUTABLE_SUFFIX}
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${CMAKE_SOURCE_DIR}/configs
                                 ${AXISSIM_PORTABLE_DIR}/axissim-data/configs
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${CMAKE_SOURCE_DIR}/reference_data
                                 ${AXISSIM_PORTABLE_DIR}/axissim-data/reference_data
                         COMMAND ${CMAKE_COMMAND} -E copy_directory
                                 ${CMAKE_BINARY_DIR}/package-assets
                                 ${AXISSIM_PORTABLE_DIR}
                         COMMENT "Bundling portable AxisSim CLI, scenarios, and reference data")
    endif()
    target_link_libraries(axissim-gui PRIVATE Qt6::Widgets axissim-core)
    # Developer builds may run the GUI from a CTest working directory that is
    # neither the source tree nor the portable application layout. Keep that
    # layout explicit while preserving the runtime search used by packages.
    target_compile_definitions(axissim-gui PRIVATE AXISSIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    axissim_enable_strict_warnings(axissim-gui)
  else()
    message(STATUS "Qt 6 Widgets not found; axissim-gui will not be built")
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS axissim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET axissim-gui AND NOT APPLE)
  install(TARGETS axissim-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY ${CMAKE_SOURCE_DIR}/configs ${CMAKE_SOURCE_DIR}/reference_data
        DESTINATION ${CMAKE_INSTALL_DATADIR}/axissim/axissim-data)
install(DIRECTORY "${CMAKE_BINARY_DIR}/package-assets/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/axissim)
if(TARGET axissim-gui AND UNIX AND NOT APPLE)
  configure_file(${CMAKE_SOURCE_DIR}/resources/axissim.desktop.in
                 ${CMAKE_BINARY_DIR}/axissim.desktop @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/axissim.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  set(CPACK_GENERATOR "TGZ;DEB")
  set(CPACK_PACKAGE_NAME "axissim")
  set(CPACK_PACKAGE_VENDOR "AxisSim")
  set(CPACK_PACKAGE_CONTACT "pakjoseph@users.noreply.github.com")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "AI architecture performance and energy exploration workbench")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(axissim-sha256-tests tests/sha256_tests.cpp)
  target_link_libraries(axissim-sha256-tests PRIVATE axissim-core)
  target_include_directories(axissim-sha256-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-sha256-tests)
  add_test(NAME sha256_cpp_contract COMMAND axissim-sha256-tests)
  add_executable(axissim-run-provenance-tests tests/run_provenance_tests.cpp)
  target_link_libraries(axissim-run-provenance-tests PRIVATE axissim-core)
  target_include_directories(axissim-run-provenance-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-run-provenance-tests)
  add_test(NAME run_provenance_cpp_contract COMMAND axissim-run-provenance-tests)
  add_executable(axissim-gemm-json-tests tests/gemm_json_tests.cpp)
  target_link_libraries(axissim-gemm-json-tests PRIVATE axissim-core)
  target_include_directories(axissim-gemm-json-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-gemm-json-tests)
  add_test(NAME gemm_json_cpp_contract COMMAND axissim-gemm-json-tests)
  add_executable(axissim-gemm-study-contract-tests tests/gemm_study_contract_tests.cpp)
  target_link_libraries(axissim-gemm-study-contract-tests PRIVATE axissim-core)
  target_include_directories(axissim-gemm-study-contract-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-gemm-study-contract-tests)
  add_test(NAME gemm_study_contract_cpp COMMAND axissim-gemm-study-contract-tests)
  add_executable(axissim-gemm-mapping-tests tests/gemm_mapping_tests.cpp)
  target_link_libraries(axissim-gemm-mapping-tests PRIVATE axissim-core)
  target_include_directories(axissim-gemm-mapping-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-gemm-mapping-tests)
  add_test(NAME gemm_mapping_cpp_contract COMMAND axissim-gemm-mapping-tests)
  add_executable(axissim-matrix-architecture-tests tests/matrix_architecture_tests.cpp)
  target_link_libraries(axissim-matrix-architecture-tests PRIVATE axissim-core)
  target_include_directories(axissim-matrix-architecture-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-matrix-architecture-tests)
  add_test(NAME matrix_architecture_cpp_contract COMMAND axissim-matrix-architecture-tests)
  add_executable(axissim-matrix-execution-tests tests/matrix_execution_tests.cpp)
  target_link_libraries(axissim-matrix-execution-tests PRIVATE axissim-core)
  target_include_directories(axissim-matrix-execution-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-matrix-execution-tests)
  add_test(NAME matrix_execution_cpp_contract COMMAND axissim-matrix-execution-tests)
  add_executable(axissim-core-model-tests tests/core_model_tests.cpp)
  target_link_libraries(axissim-core-model-tests PRIVATE axissim-core)
  target_include_directories(axissim-core-model-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  axissim_enable_strict_warnings(axissim-core-model-tests)
  add_test(NAME core_model_api_contract COMMAND axissim-core-model-tests)
  add_executable(axissim-scheduler-ordering-tests tests/scheduler_ordering_tests.cpp)
  target_link_libraries(axissim-scheduler-ordering-tests PRIVATE axissim-core)
  target_include_directories(axissim-scheduler-ordering-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(axissim-scheduler-ordering-tests PRIVATE AXISSIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  axissim_enable_strict_warnings(axissim-scheduler-ordering-tests)
  add_test(NAME scheduler_ordering_cpp_contract COMMAND axissim-scheduler-ordering-tests)
  add_executable(axissim-study-measurement-tests tests/study_measurement_tests.cpp)
  target_link_libraries(axissim-study-measurement-tests PRIVATE axissim-core)
  target_include_directories(axissim-study-measurement-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(axissim-study-measurement-tests PRIVATE AXISSIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  axissim_enable_strict_warnings(axissim-study-measurement-tests)
  add_test(NAME study_measurement_contract COMMAND axissim-study-measurement-tests)
  add_executable(axissim-study-contract-tests tests/study_contract_tests.cpp)
  target_link_libraries(axissim-study-contract-tests PRIVATE axissim-core)
  target_include_directories(axissim-study-contract-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(axissim-study-contract-tests PRIVATE AXISSIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  axissim_enable_strict_warnings(axissim-study-contract-tests)
  add_test(NAME study_candidate_accounting_contract COMMAND axissim-study-contract-tests)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME host_reference_harness_help
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/benchmark_onnx_cpu.py --help)
    add_test(NAME host_reference_records_reconcile
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_host_reference_records.py
                     ${CMAKE_SOURCE_DIR}/reference_data/hosts/apple-silicon-arm64-mobilenetv2-7.json
                     ${CMAKE_SOURCE_DIR}/reference_data/hosts/github-actions-ubuntu-x86_64-mobilenetv2-7.json)
    if(Python3_VERSION VERSION_GREATER_EQUAL "3.10")
      message(STATUS "AxisSim tests: Python ${Python3_VERSION} enables the full Python 3.10+ study and provenance contract suite")
      add_test(NAME power_cost_coverage_documentation_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/power_cost_coverage_docs.py
                       --source-root ${CMAKE_SOURCE_DIR})
      add_test(NAME m5_replay_verifier_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/m5_replay_verifier_tests.py)
      add_test(NAME study_manifest_preflight_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/study_runner_tests.py
                       --source-root ${CMAKE_SOURCE_DIR}
                       --binary $<TARGET_FILE:axissim>)
      add_test(NAME study_candidate_cli_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/study_contract_e2e.py
                       --source-root ${CMAKE_SOURCE_DIR}
                       --binary $<TARGET_FILE:axissim>)
      add_test(NAME gemm_pair_cli_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/gemm_study_contract_e2e.py
                       --binary $<TARGET_FILE:axissim>)
      add_test(NAME matrix_mapping_study_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/matrix_mapping_study_tests.py
                       --source-root ${CMAKE_SOURCE_DIR}
                       --binary $<TARGET_FILE:axissim>)
      add_test(NAME matrix_mapping_study_e2e
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/matrix_mapping_study_e2e.py
                       --source-root ${CMAKE_SOURCE_DIR}
                       --binary $<TARGET_FILE:axissim>)
      add_test(NAME study_m5_execution_pareto_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/study_m5_tests.py
                       --source-root ${CMAKE_SOURCE_DIR}
                       --binary $<TARGET_FILE:axissim>)
      if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        add_test(NAME legacy_study_baselines_reproduce
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_legacy_study_parity.py
                         --source-root ${CMAKE_SOURCE_DIR}
                         --binary $<TARGET_FILE:axissim>
                         --fixture ${CMAKE_SOURCE_DIR}/tests/reference/legacy_study_baselines.json)
      else()
        add_test(NAME legacy_study_baselines_repeat_per_platform
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_legacy_study_parity.py
                         --source-root ${CMAKE_SOURCE_DIR}
                         --binary $<TARGET_FILE:axissim>
                         --fixture ${CMAKE_SOURCE_DIR}/tests/reference/legacy_study_baselines.json
                         --repeat-only)
      endif()
      add_test(NAME scheduler_ordering_deterministic_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_deterministic_scheduler.py
                       --source-root ${CMAKE_SOURCE_DIR}
                       --binary $<TARGET_FILE:axissim>
                       --fixture ${CMAKE_SOURCE_DIR}/tests/reference/deterministic_scheduler_v1.json)
      add_test(NAME mnist12_study_power_overlay_preserves_legacy_config
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/check_mnist12_power_overlay.py
                       ${CMAKE_SOURCE_DIR} $<TARGET_FILE:axissim>)
      add_test(NAME run_provenance_cli_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_provenance_e2e.py
                       --source-root ${CMAKE_SOURCE_DIR} --binary $<TARGET_FILE:axissim>)
      add_test(NAME package_metadata_contract
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/package_metadata_tests.py
                       --source-root ${CMAKE_SOURCE_DIR} --build-dir ${CMAKE_BINARY_DIR})
    endif()
    if(NOT Python3_VERSION VERSION_GREATER_EQUAL "3.10")
      message(WARNING "AxisSim tests: Python ${Python3_VERSION} is older than 3.10; study, provenance, and full runner contracts are not registered. Install Python 3.10+ and reconfigure for the complete test suite.")
    endif()
  else()
    message(WARNING "AxisSim tests: Python was not found; Python-backed tests are not registered. Install Python 3.10+ and reconfigure for the complete study and provenance test suite.")
  endif()

  if(Python3_Interpreter_FOUND AND Python3_VERSION VERSION_GREATER_EQUAL "3.10")
    set(AXISSIM_REGISTRATION_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
  else()
    set(AXISSIM_REGISTRATION_PYTHON_EXECUTABLE "")
  endif()
  add_test(NAME cmake_python_registration_contract
           COMMAND ${CMAKE_COMMAND}
                   -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                   -DTEST_ROOT=${CMAKE_BINARY_DIR}/python-registration-check
                   -DPYTHON_EXECUTABLE=${AXISSIM_REGISTRATION_PYTHON_EXECUTABLE}
                   -P ${CMAKE_SOURCE_DIR}/tests/cmake_python_registration_tests.cmake)

  add_test(NAME baseline_smoke
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf)
  add_test(NAME rejects_invalid_rvv_configuration
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/invalid_rvv.conf)
  set_tests_properties(rejects_invalid_rvv_configuration PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rejects_unknown_configuration_key
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/unknown_key.conf)
  set_tests_properties(rejects_unknown_configuration_key PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rejects_unknown_scheduler_ordering_policy
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --set scheduler_ordering_policy=platform_default)
  set_tests_properties(rejects_unknown_scheduler_ordering_policy PROPERTIES WILL_FAIL TRUE)
  add_test(NAME validates_workload_profile_contract
           COMMAND axissim profile-validate ${CMAKE_SOURCE_DIR}/tests/reference/tiny_workload_profile.json)
  add_test(NAME validates_explicit_mnist_mapping_plan
           COMMAND axissim profile-validate ${CMAKE_SOURCE_DIR}/reference_data/workloads/mnist12_profile.json
                   ${CMAKE_SOURCE_DIR}/configs/mappings/mnist12_baseline.json)
  add_test(NAME rejects_incomplete_mapping_plan
           COMMAND axissim profile-validate ${CMAKE_SOURCE_DIR}/reference_data/workloads/mnist12_profile.json
                   ${CMAKE_SOURCE_DIR}/tests/reference/invalid_mapping_missing_operator.json)
  set_tests_properties(rejects_incomplete_mapping_plan PROPERTIES WILL_FAIL TRUE)
  add_test(NAME executes_workload_profile_dag
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_profile.conf)
  set_tests_properties(executes_workload_profile_dag PROPERTIES
                       PASS_REGULAR_EXPRESSION "latency mean / P99:  0.30 / 0.30 us")
  add_test(NAME mnist12_profiled_graph_smoke
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_profiled.conf)
  set_tests_properties(mnist12_profiled_graph_smoke PROPERTIES
                       PASS_REGULAR_EXPRESSION "workload mode:        profiled_graph")
  add_test(NAME explicit_mapping_preserves_mnist_baseline
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_profiled.conf)
  set_tests_properties(explicit_mapping_preserves_mnist_baseline PROPERTIES
                       PASS_REGULAR_EXPRESSION "latency mean / P99:  56.42 / 56.51 us")
  add_test(NAME profile_transfer_ledger_reconciles
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_profile_transfer_ledger.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_profiled.conf)
  add_test(NAME named_profile_fusion_reduces_only_declared_traffic
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_profile_fusion.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_profiled.conf
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_plus_relu_fused.conf)
  add_test(NAME named_profile_weight_residency_accounts_for_first_and_steady_frames
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_profile_persistent_weights.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_profiled.conf
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_persistent_weights.conf)
  add_test(NAME rejects_profile_weight_residency_that_exceeds_declared_sram
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_persistent_weights.conf
                   --set weight_sram_capacity_bytes=1)
  set_tests_properties(rejects_profile_weight_residency_that_exceeds_declared_sram PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rejects_named_profile_weight_residency_with_actionable_capacity_error
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_rejected_profile_residency.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_persistent_weights_insufficient_sram.conf)
  add_test(NAME combined_profile_mapping_policy_smoke
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_fused_persistent_weights.conf)
  set_tests_properties(combined_profile_mapping_policy_smoke PROPERTIES
                       PASS_REGULAR_EXPRESSION "fused intermediate traffic avoided: 50176 bytes / frame")
  add_test(NAME profile_precision_policy_scales_only_declared_storage_and_npu_energy
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_profile_precision_policy.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_profiled.conf
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_fp16_storage.conf)
  add_test(NAME controlled_int8_mapping_binds_external_accuracy_evidence
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_controlled_int8_evidence.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/workloads/mnist12_controlled_int8.conf)
  add_test(NAME mnist12_profiled_burst_qos_smoke
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/qos/mnist12_burst.conf)
  set_tests_properties(mnist12_profiled_burst_qos_smoke PROPERTIES
                       PASS_REGULAR_EXPRESSION "frame arrivals:       trace")
  add_test(NAME rejects_incomplete_arrival_trace_configuration
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/invalid_arrival_trace.conf)
  set_tests_properties(rejects_incomplete_arrival_trace_configuration PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rejects_tile_that_does_not_fit_sram
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/insufficient_sram.conf)
  set_tests_properties(rejects_tile_that_does_not_fit_sram PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rejects_weight_residency_that_does_not_fit_sram
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/insufficient_weight_sram.conf)
  set_tests_properties(rejects_weight_residency_that_does_not_fit_sram PROPERTIES WILL_FAIL TRUE)
  add_test(NAME bandwidth_sweep_smoke
           COMMAND axissim sweep ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf --sweep dram_bandwidth_gbps=0.15,20.0)
  add_test(NAME fixed_priority_control_smoke
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf --set control_period_us=1000 --set control_deadline_us=250 --set control_ops=400)
  add_test(NAME edf_control_smoke
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf --set scheduler_policy=edf --set control_period_us=1000 --set control_deadline_us=250 --set control_ops=400)
  add_test(NAME vector_chunking_bounds_control_blocking
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf --set frames=20 --set frame_period_us=500 --set deadline_us=500 --set control_period_us=200 --set control_deadline_us=80 --set control_ops=200000 --set rvv_chunk_elements=20000)
  set_tests_properties(vector_chunking_bounds_control_blocking PROPERTIES
                       PASS_REGULAR_EXPRESSION "control deadline misses: 0 / 50")
  add_test(NAME tiny_pipeline_matches_hand_calculation
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf)
  set_tests_properties(tiny_pipeline_matches_hand_calculation PROPERTIES
                       PASS_REGULAR_EXPRESSION "latency mean / P99:  0.28 / 0.28 us")
  add_test(NAME rvv_extra_traffic_is_accounted_once
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --set rvv_memory_accounting=extra_traffic --set rvv_memory_bytes_per_element=4)
  set_tests_properties(rvv_extra_traffic_is_accounted_once PROPERTIES
                       PASS_REGULAR_EXPRESSION "DRAM traffic:        304.09 MB")
  add_test(NAME rvv_extra_traffic_covers_pre_and_postprocess
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf
                   --set rvv_memory_accounting=extra_traffic --set rvv_memory_bytes_per_element=1000)
  set_tests_properties(rvv_extra_traffic_covers_pre_and_postprocess PROPERTIES
                       PASS_REGULAR_EXPRESSION "DRAM traffic:        0.01 MB")
  add_test(NAME pue_scales_facility_power_only
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf
                   --set static_power_w=1 --set pue=1.5)
  set_tests_properties(pue_scales_facility_power_only PROPERTIES
                       PASS_REGULAR_EXPRESSION "average IT/facility power: 1.00 / 1.50 W")
  add_test(NAME reports_infeasible_it_power_budget
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf
                   --set static_power_w=1 --set it_power_budget_w=0.001)
  set_tests_properties(reports_infeasible_it_power_budget PROPERTIES
                       PASS_REGULAR_EXPRESSION "infeasible")
  add_test(NAME iso_performance_arithmetic_scale_preserves_latency
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --set npu_arithmetic_energy_scale=0.333333)
  set_tests_properties(iso_performance_arithmetic_scale_preserves_latency PROPERTIES
                       PASS_REGULAR_EXPRESSION "latency mean / P99:  861.06 / 861.06 us")
  add_test(NAME larger_npu_has_explicit_capacity_power
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf
                   --set static_power_w=0 --set npu_macs=4 --set npu_static_power_per_mac_w=0.1)
  set_tests_properties(larger_npu_has_explicit_capacity_power PROPERTIES
                       PASS_REGULAR_EXPRESSION "modeled static power: 0.40 W")
  add_test(NAME exact_power_accounting
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_power_accounting.sh $<TARGET_FILE:axissim> ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf)
  add_test(NAME pareto_filter_rejects_dominated_and_infeasible
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_pareto_filter.sh
                   ${CMAKE_SOURCE_DIR}/experiments/analyze_capacity_sweep.sh
                   ${CMAKE_SOURCE_DIR}/tests/fixtures/pareto_capacity_sweep.csv)
  add_test(NAME deployment_economics_are_explicit
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf --set static_power_w=1 --set pue=1.5 --set deployment_xpu_count=1000 --set deployment_hours=1 --set electricity_cost_per_kwh=0.1)
  set_tests_properties(deployment_economics_are_explicit PROPERTIES PASS_REGULAR_EXPRESSION "1.50 kWh / 0.15")
  add_test(NAME implementation_proxies_are_explicit
           COMMAND axissim run ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf --set static_power_w=1 --set base_area_mm2=1 --set npu_area_per_mac_mm2=0.1 --set max_area_mm2=2 --set thermal_resistance_c_per_w=10 --set thermal_capacitance_j_per_c=1 --set max_junction_temp_c=100 --set reliability_fit_rate=100 --set deployment_xpu_count=1000 --set deployment_hours=1000)
  set_tests_properties(implementation_proxies_are_explicit PROPERTIES PASS_REGULAR_EXPRESSION "reliability proxy expected failures: 0.10")
  add_test(NAME exact_deployment_and_proxy_accounting
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_deployment_and_proxies.sh $<TARGET_FILE:axissim> ${CMAKE_SOURCE_DIR}/tests/tiny_pipeline.conf)
  add_test(NAME deterministic_arrival_trace_qos
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_arrival_trace.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   ${CMAKE_SOURCE_DIR}/configs/qos/periodic_trace.conf
                   ${CMAKE_SOURCE_DIR}/configs/qos/synthetic_burst.conf)
  add_test(NAME deterministic_sensitivity_analysis
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_sensitivity_analysis.sh $<TARGET_FILE:axissim>
                   ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   ${CMAKE_SOURCE_DIR}/tests/reference/sensitivity_ranges.csv)
  add_test(NAME calibration_fixture_smoke
           COMMAND axissim calibrate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --reference ${CMAKE_SOURCE_DIR}/tests/reference/synthetic_latency.csv)
  set_tests_properties(calibration_fixture_smoke PROPERTIES
                       PASS_REGULAR_EXPRESSION "synthetic_2,reproduced,.,2")
  add_test(NAME rejects_calibration_frequency_mismatch
           COMMAND axissim calibrate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --reference ${CMAKE_SOURCE_DIR}/tests/reference/mismatched_frequency.csv)
  set_tests_properties(rejects_calibration_frequency_mismatch PROPERTIES WILL_FAIL TRUE)
  add_test(NAME ara2_published_reference_smoke
           COMMAND axissim calibrate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --reference ${CMAKE_SOURCE_DIR}/reference_data/ara/ara2_published_latency.csv)
  set_tests_properties(ara2_published_reference_smoke PROPERTIES
                       PASS_REGULAR_EXPRESSION "ara2_table_iv_fmatmul64_4lane,published")
  add_test(NAME ara_rtl_reproduced_reference_smoke
           COMMAND axissim calibrate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --reference ${CMAKE_SOURCE_DIR}/reference_data/ara/ara_rtl_reproduced_latency.csv)
  set_tests_properties(ara_rtl_reproduced_reference_smoke PROPERTIES
                        PASS_REGULAR_EXPRESSION "ara_rtl_fmatmul64_128_4lane,reproduced,../../configs/calibration/ara_fmatmul64_128_4lane.conf,4,532392.000,524288.000")
  add_test(NAME ara_rtl_4lane_size_controls_smoke
           COMMAND axissim calibrate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --reference ${CMAKE_SOURCE_DIR}/reference_data/ara/ara_rtl_4lane_size_controls.csv)
  set_tests_properties(ara_rtl_4lane_size_controls_smoke PROPERTIES
                        PASS_REGULAR_EXPRESSION "ara_rtl_fmatmul64_128_4lane_control_rerun,reproduced,../../configs/calibration/ara_fmatmul64_128_4lane.conf,4,532392.000,524288.000")
  add_test(NAME ara_rtl_lane_holdout_smoke
           COMMAND axissim calibrate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf
                   --reference ${CMAKE_SOURCE_DIR}/reference_data/ara/ara_rtl_lane_holdout.csv)
  set_tests_properties(ara_rtl_lane_holdout_smoke PROPERTIES
                        PASS_REGULAR_EXPRESSION "ara_rtl_fmatmul64_128_2lane_holdout,reproduced,../../configs/calibration/ara_fmatmul64_128_2lane.conf,2,1064052.000,1048576.000")
  if(TARGET axissim-gui)
    add_test(NAME desktop_gui_initializes COMMAND axissim-gui --smoke-test)
    set_tests_properties(desktop_gui_initializes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME desktop_gui_runs_model COMMAND axissim-gui --smoke-test-run)
    set_tests_properties(desktop_gui_runs_model PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    add_test(NAME desktop_gui_results_workbench_contract COMMAND axissim-gui --results-workbench-smoke-test-run)
    set_tests_properties(desktop_gui_results_workbench_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
    add_test(NAME desktop_gui_guided_first_run COMMAND axissim-gui --tutorial-smoke-test-run)
    set_tests_properties(desktop_gui_guided_first_run PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    add_test(NAME desktop_gui_mapping_lab_runs_public_policy_set COMMAND axissim-gui --mapping-lab-smoke-test-run)
    set_tests_properties(desktop_gui_mapping_lab_runs_public_policy_set PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    add_test(NAME desktop_gui_power_opportunity_study_runs COMMAND axissim-gui --power-study-smoke-test-run)
    set_tests_properties(desktop_gui_power_opportunity_study_runs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    add_test(NAME desktop_gui_accessibility_audit COMMAND axissim-gui --audit-ui)
    set_tests_properties(desktop_gui_accessibility_audit PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME desktop_gui_light_theme COMMAND axissim-gui --smoke-test)
    set_tests_properties(desktop_gui_light_theme PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen;AXISSIM_APPEARANCE_OVERRIDE=light")
    add_test(NAME desktop_gui_dark_theme COMMAND axissim-gui --smoke-test)
    set_tests_properties(desktop_gui_dark_theme PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen;AXISSIM_APPEARANCE_OVERRIDE=dark")
    if(NOT APPLE)
      add_test(NAME desktop_gui_portable_layout_runs_model
               COMMAND ${AXISSIM_PORTABLE_DIR}/AxisSim${CMAKE_EXECUTABLE_SUFFIX} --smoke-test-run)
      set_tests_properties(desktop_gui_portable_layout_runs_model PROPERTIES
                           ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    endif()
  endif()
endif()
