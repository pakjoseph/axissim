#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace axissim {
namespace {

[[nodiscard]] std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

[[nodiscard]] std::map<std::string, std::string> read_key_values_impl(
    const std::filesystem::path& path, std::set<std::string>& active_paths) {
  const std::string canonical_path = std::filesystem::absolute(path).lexically_normal().string();
  if (!active_paths.insert(canonical_path).second) throw std::runtime_error("Configuration include cycle: " + canonical_path);
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Unable to open config: " + path.string());
  std::map<std::string, std::string> values;
  std::string line;
  int line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    if (line.empty()) continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) throw std::runtime_error("Expected key = value at " + path.string() + ":" + std::to_string(line_number));
    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));
    if (key.empty() || value.empty()) throw std::runtime_error("Invalid key/value at " + path.string() + ":" + std::to_string(line_number));
    if (key == "include") {
      const auto included = read_key_values_impl(path.parent_path() / value, active_paths);
      for (const auto& [included_key, included_value] : included) {
        if (!values.emplace(included_key, included_value).second) throw std::runtime_error("Duplicate key from include: " + included_key);
      }
    } else if (!values.emplace(key, value).second) {
      throw std::runtime_error("Duplicate key at " + path.string() + ":" + std::to_string(line_number));
    }
  }
  active_paths.erase(canonical_path);
  return values;
}

[[nodiscard]] std::map<std::string, std::string> read_key_values(const std::string& path) {
  std::set<std::string> active_paths;
  return read_key_values_impl(path, active_paths);
}

template <typename T>
[[nodiscard]] T parse_number(const std::map<std::string, std::string>& values, const std::string& key, T fallback) {
  const auto it = values.find(key);
  if (it == values.end()) return fallback;
  std::stringstream converter(it->second);
  T result{};
  converter >> result;
  if (!converter || !converter.eof()) throw std::runtime_error("Invalid numeric value for " + key);
  return result;
}

}  // namespace

const std::set<std::string>& supported_keys() {
  static const std::set<std::string> keys = {
      "scenario_name", "model_version", "workload_mode", "workload_profile", "mapping_plan", "scheduler_policy", "frame_arrival_mode", "frame_arrival_trace", "frame_arrival_provenance", "frames", "frame_period_us", "deadline_us", "dram_bandwidth_gbps", "dma_setup_us", "control_period_us", "control_deadline_us", "control_ops",
      "rvv_vlen_bits", "rvv_sew_bits", "rvv_lmul", "rvv_lanes", "rvv_ops_per_lane_cycle", "rvv_frequency_ghz", "rvv_stripmine_overhead_cycles", "rvv_timing_model", "rvv_affine_overhead_cycles", "rvv_compute_cycle_scale", "rvv_chunk_elements", "rvv_memory_accounting", "rvv_memory_bytes_per_element",
      "npu_macs", "npu_frequency_ghz", "input_bytes", "preprocess_elements", "preprocess_ops_per_element", "preprocess_output_bytes",
      "gemm_m", "gemm_n", "gemm_k", "npu_tile_m", "npu_tile_n", "npu_tile_k",
      "npu_element_bytes", "npu_accumulator_bytes", "sram_capacity_bytes", "npu_weights_resident", "weight_sram_capacity_bytes", "dma_double_buffering",
      "postprocess_elements", "postprocess_ops_per_element", "postprocess_bytes", "vector_op_pj", "npu_mac_pj",
      "npu_arithmetic_energy_scale", "arithmetic_voltage_ratio", "dram_byte_pj", "dma_byte_pj", "static_power_w", "npu_static_power_per_mac_w", "rvv_static_power_per_lane_w", "sram_static_power_per_kib_w", "dram_interface_static_power_w", "it_power_budget_w", "pue", "electricity_cost_per_kwh", "deployment_xpu_count", "deployment_hours", "base_area_mm2", "npu_area_per_mac_mm2", "rvv_area_per_lane_mm2", "sram_area_per_kib_mm2", "max_area_mm2", "ambient_temp_c", "thermal_resistance_c_per_w", "thermal_capacitance_j_per_c", "max_junction_temp_c", "reliability_fit_rate", "reliability_voltage_fit_multiplier"};
  return keys;
}

Config load_config(const std::string& path, const std::map<std::string, std::string>& overrides) {
  auto values = read_key_values(path);
  for (const auto& [key, value] : overrides) values[key] = value;
  for (const auto& [key, value] : values) {
    (void)value;
    if (!supported_keys().contains(key)) throw std::runtime_error("Unsupported configuration key: " + key);
  }
  Config c;
  if (values.contains("scenario_name")) c.scenario_name = values.at("scenario_name");
  if (values.contains("model_version")) c.model_version = values.at("model_version");
  if (values.contains("workload_mode")) c.workload_mode = values.at("workload_mode");
  if (values.contains("workload_profile")) c.workload_profile = (std::filesystem::path(path).parent_path() / values.at("workload_profile")).lexically_normal().string();
  if (values.contains("mapping_plan")) c.mapping_plan = (std::filesystem::path(path).parent_path() / values.at("mapping_plan")).lexically_normal().string();
  if (values.contains("scheduler_policy")) c.scheduler_policy = values.at("scheduler_policy");
  if (values.contains("frame_arrival_mode")) c.frame_arrival_mode = values.at("frame_arrival_mode");
  if (values.contains("frame_arrival_trace")) c.frame_arrival_trace = (std::filesystem::path(path).parent_path() / values.at("frame_arrival_trace")).lexically_normal().string();
  if (values.contains("frame_arrival_provenance")) c.frame_arrival_provenance = (std::filesystem::path(path).parent_path() / values.at("frame_arrival_provenance")).lexically_normal().string();
  if (values.contains("rvv_timing_model")) c.rvv_timing_model = values.at("rvv_timing_model");
  if (values.contains("rvv_memory_accounting")) c.rvv_memory_accounting = values.at("rvv_memory_accounting");
#define AXISSIM_LOAD(field) c.field = parse_number(values, #field, c.field)
  AXISSIM_LOAD(frames); AXISSIM_LOAD(frame_period_us); AXISSIM_LOAD(deadline_us);
  AXISSIM_LOAD(dram_bandwidth_gbps); AXISSIM_LOAD(dma_setup_us); AXISSIM_LOAD(control_period_us); AXISSIM_LOAD(control_deadline_us); AXISSIM_LOAD(control_ops);
  AXISSIM_LOAD(rvv_vlen_bits); AXISSIM_LOAD(rvv_sew_bits); AXISSIM_LOAD(rvv_lmul); AXISSIM_LOAD(rvv_lanes);
  AXISSIM_LOAD(rvv_ops_per_lane_cycle); AXISSIM_LOAD(rvv_frequency_ghz); AXISSIM_LOAD(rvv_stripmine_overhead_cycles); AXISSIM_LOAD(rvv_affine_overhead_cycles); AXISSIM_LOAD(rvv_compute_cycle_scale); AXISSIM_LOAD(rvv_chunk_elements); AXISSIM_LOAD(rvv_memory_bytes_per_element);
  AXISSIM_LOAD(npu_macs); AXISSIM_LOAD(npu_frequency_ghz);
  AXISSIM_LOAD(input_bytes); AXISSIM_LOAD(preprocess_elements); AXISSIM_LOAD(preprocess_ops_per_element); AXISSIM_LOAD(preprocess_output_bytes);
  AXISSIM_LOAD(gemm_m); AXISSIM_LOAD(gemm_n); AXISSIM_LOAD(gemm_k);
  AXISSIM_LOAD(npu_tile_m); AXISSIM_LOAD(npu_tile_n); AXISSIM_LOAD(npu_tile_k);
  AXISSIM_LOAD(npu_element_bytes); AXISSIM_LOAD(npu_accumulator_bytes); AXISSIM_LOAD(sram_capacity_bytes);
  AXISSIM_LOAD(npu_weights_resident); AXISSIM_LOAD(weight_sram_capacity_bytes); AXISSIM_LOAD(dma_double_buffering);
  AXISSIM_LOAD(postprocess_elements); AXISSIM_LOAD(postprocess_ops_per_element); AXISSIM_LOAD(postprocess_bytes);
  AXISSIM_LOAD(vector_op_pj); AXISSIM_LOAD(npu_mac_pj); AXISSIM_LOAD(npu_arithmetic_energy_scale); AXISSIM_LOAD(arithmetic_voltage_ratio);
  AXISSIM_LOAD(dram_byte_pj); AXISSIM_LOAD(dma_byte_pj);
  AXISSIM_LOAD(static_power_w); AXISSIM_LOAD(npu_static_power_per_mac_w); AXISSIM_LOAD(rvv_static_power_per_lane_w); AXISSIM_LOAD(sram_static_power_per_kib_w); AXISSIM_LOAD(dram_interface_static_power_w); AXISSIM_LOAD(it_power_budget_w); AXISSIM_LOAD(pue); AXISSIM_LOAD(electricity_cost_per_kwh); AXISSIM_LOAD(deployment_xpu_count); AXISSIM_LOAD(deployment_hours);
  AXISSIM_LOAD(base_area_mm2); AXISSIM_LOAD(npu_area_per_mac_mm2); AXISSIM_LOAD(rvv_area_per_lane_mm2); AXISSIM_LOAD(sram_area_per_kib_mm2); AXISSIM_LOAD(max_area_mm2); AXISSIM_LOAD(ambient_temp_c); AXISSIM_LOAD(thermal_resistance_c_per_w); AXISSIM_LOAD(thermal_capacitance_j_per_c); AXISSIM_LOAD(max_junction_temp_c); AXISSIM_LOAD(reliability_fit_rate); AXISSIM_LOAD(reliability_voltage_fit_multiplier);
#undef AXISSIM_LOAD
  return c;
}

}  // namespace axissim
