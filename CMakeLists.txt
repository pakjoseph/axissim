cmake_minimum_required(VERSION 3.20)
project(axissim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(axissim src/main.cpp)
target_compile_options(axissim PRIVATE -Wall -Wextra -Wpedantic -Werror)

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
endif()
