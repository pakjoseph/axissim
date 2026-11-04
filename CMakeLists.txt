cmake_minimum_required(VERSION 3.20)
project(axissim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(axissim src/main.cpp)
target_compile_options(axissim PRIVATE -Wall -Wextra -Wpedantic -Werror)

option(AXISSIM_BUILD_GUI "Build the Qt 6 desktop explorer when Qt Widgets is available" ON)
if(AXISSIM_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6Widgets_FOUND)
    add_executable(axissim-gui src/gui/main.cpp)
    target_link_libraries(axissim-gui PRIVATE Qt6::Widgets)
    target_compile_options(axissim-gui PRIVATE -Wall -Wextra -Wpedantic -Werror)
  else()
    message(STATUS "Qt 6 Widgets not found; axissim-gui will not be built")
  endif()
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
endif()
