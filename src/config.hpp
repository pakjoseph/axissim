#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace axissim {

// All model inputs are explicit, versioned scenario data rather than hidden constants.
struct Config {
  std::string scenario_name = "unnamed";
  std::string model_version = "0.1";
  std::string workload_mode = "physical_ai";
  std::string scheduler_policy = "fixed_priority";
  int frames = 30;
  double frame_period_us = 33'333.0;
  double deadline_us = 33'333.0;
  double dram_bandwidth_gbps = 20.0;
  double dma_setup_us = 3.0;
  double control_period_us = 0.0;
  double control_deadline_us = 0.0;
  std::uint64_t control_ops = 0;
  int rvv_vlen_bits = 256;
  int rvv_sew_bits = 32;
  int rvv_lmul = 1;
  int rvv_lanes = 8;
  int rvv_ops_per_lane_cycle = 1;
  double rvv_frequency_ghz = 1.0;
  double rvv_stripmine_overhead_cycles = 1.0;
  std::string rvv_timing_model = "compute_envelope";
  double rvv_affine_overhead_cycles = 0.0;
  double rvv_compute_cycle_scale = 1.0;
  std::uint64_t rvv_chunk_elements = 0;
  std::string rvv_memory_accounting = "pipeline_dma";
  int rvv_memory_bytes_per_element = 0;
  int npu_macs = 1024;
  double npu_frequency_ghz = 1.0;
  std::uint64_t input_bytes = 921'600;
  std::uint64_t preprocess_elements = 921'600;
  int preprocess_ops_per_element = 3;
  std::uint64_t preprocess_output_bytes = 1'843'200;
  int gemm_m = 256;
  int gemm_n = 256;
  int gemm_k = 512;
  int npu_tile_m = 64;
  int npu_tile_n = 64;
  int npu_tile_k = 128;
  int npu_element_bytes = 2;
  int npu_accumulator_bytes = 4;
  std::uint64_t sram_capacity_bytes = 131'072;
  int npu_weights_resident = 0;
  std::uint64_t weight_sram_capacity_bytes = 0;
  int dma_double_buffering = 1;
  std::uint64_t postprocess_elements = 100'001;
  int postprocess_ops_per_element = 3;
  std::uint64_t postprocess_bytes = 4'096;
  double vector_op_pj = 0.40;
  double npu_mac_pj = 0.20;
  double npu_arithmetic_energy_scale = 1.0;
  double arithmetic_voltage_ratio = 1.0;
  double dram_byte_pj = 15.0;
  double dma_byte_pj = 1.5;
  double static_power_w = 0.0;
  double npu_static_power_per_mac_w = 0.0;
  double rvv_static_power_per_lane_w = 0.0;
  double sram_static_power_per_kib_w = 0.0;
  double dram_interface_static_power_w = 0.0;
  double it_power_budget_w = 0.0;
  double pue = 1.0;
};

Config load_config(const std::string& path, const std::map<std::string, std::string>& overrides = {});
void validate(const Config& config);

}  // namespace axissim
