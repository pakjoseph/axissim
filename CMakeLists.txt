cmake_minimum_required(VERSION 3.20)
project(axissim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(axissim src/main.cpp src/config.cpp src/profile.cpp)
function(axissim_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()
axissim_enable_strict_warnings(axissim)

option(AXISSIM_BUILD_GUI "Build the Qt 6 desktop explorer when Qt Widgets is available" ON)
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
                         COMMENT "Bundling portable AxisSim CLI, scenarios, and reference data")
    endif()
    target_link_libraries(axissim-gui PRIVATE Qt6::Widgets)
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
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6, libqt6gui6, libqt6core6")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME baseline_smoke
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/configs/baseline/manifest.conf)
  add_test(NAME rejects_invalid_rvv_configuration
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/invalid_rvv.conf)
  set_tests_properties(rejects_invalid_rvv_configuration PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rejects_unknown_configuration_key
           COMMAND axissim validate ${CMAKE_SOURCE_DIR}/tests/unknown_key.conf)
  set_tests_properties(rejects_unknown_configuration_key PROPERTIES WILL_FAIL TRUE)
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
           COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_pareto_filter.sh ${CMAKE_SOURCE_DIR}/experiments/analyze_capacity_sweep.sh)
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
