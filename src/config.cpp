#include "config.hpp"

#include <algorithm>
#include <cmath>
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


void validate(const Config& c) {
  if (c.frames < 1 || c.frame_period_us <= 0 || c.deadline_us <= 0) throw std::runtime_error("Frame configuration must be positive");
  if (c.workload_mode != "physical_ai" && c.workload_mode != "rvv_stream" && c.workload_mode != "profiled_graph") throw std::runtime_error("workload_mode must be physical_ai, rvv_stream, or profiled_graph");
  if (c.workload_mode == "profiled_graph" && c.workload_profile.empty()) throw std::runtime_error("profiled_graph workloads require workload_profile");
  if (c.workload_mode != "profiled_graph" && !c.workload_profile.empty()) throw std::runtime_error("workload_profile is only valid with workload_mode=profiled_graph");
  if (c.workload_mode != "profiled_graph" && !c.mapping_plan.empty()) throw std::runtime_error("mapping_plan is only valid with workload_mode=profiled_graph");
  if (c.frame_arrival_mode != "periodic" && c.frame_arrival_mode != "trace") throw std::runtime_error("frame_arrival_mode must be periodic or trace");
  if (c.frame_arrival_mode == "periodic" && (!c.frame_arrival_trace.empty() || !c.frame_arrival_provenance.empty())) throw std::runtime_error("Periodic arrivals must not specify trace or provenance files");
  if (c.frame_arrival_mode == "trace" && (c.frame_arrival_trace.empty() || c.frame_arrival_provenance.empty())) throw std::runtime_error("Trace arrivals require frame_arrival_trace and frame_arrival_provenance");
  if (c.rvv_timing_model != "compute_envelope" && c.rvv_timing_model != "affine_overhead") throw std::runtime_error("rvv_timing_model must be compute_envelope or affine_overhead");
  if (c.rvv_memory_accounting != "pipeline_dma" && c.rvv_memory_accounting != "extra_traffic") throw std::runtime_error("rvv_memory_accounting must be pipeline_dma or extra_traffic");
  if (c.scheduler_policy != "fcfs" && c.scheduler_policy != "fixed_priority" && c.scheduler_policy != "edf") throw std::runtime_error("scheduler_policy must be fcfs, fixed_priority, or edf");
  if (c.control_period_us < 0 || c.control_deadline_us < 0 || (c.control_period_us == 0 && (c.control_deadline_us != 0 || c.control_ops != 0)) || (c.control_period_us > 0 && (c.control_deadline_us <= 0 || c.control_ops == 0))) throw std::runtime_error("Control workload must be fully disabled or have positive period, deadline, and operation count");
  if (c.dram_bandwidth_gbps <= 0 || c.rvv_frequency_ghz <= 0 || c.npu_frequency_ghz <= 0) throw std::runtime_error("Bandwidth and frequencies must be positive");
  if (c.rvv_vlen_bits <= 0 || c.rvv_sew_bits <= 0 || c.rvv_vlen_bits % c.rvv_sew_bits != 0 || c.rvv_lmul < 1 || c.rvv_lanes < 1 || c.rvv_ops_per_lane_cycle < 1 || c.rvv_stripmine_overhead_cycles < 0 || c.rvv_affine_overhead_cycles < 0 || c.rvv_compute_cycle_scale <= 0 || c.rvv_memory_bytes_per_element < 0 || c.preprocess_ops_per_element < 1 || c.postprocess_ops_per_element < 1) throw std::runtime_error("Invalid RVV configuration");
  if (c.npu_macs < 1 || c.gemm_m < 1 || c.gemm_n < 1 || c.gemm_k < 1 || c.npu_tile_m < 1 || c.npu_tile_n < 1 || c.npu_tile_k < 1 || c.npu_element_bytes < 1 || c.npu_accumulator_bytes < 1) throw std::runtime_error("NPU dimensions, tiles, element sizes, and MAC count must be positive");
  const auto tile_m = static_cast<std::uint64_t>(std::min(c.gemm_m, c.npu_tile_m));
  const auto tile_n = static_cast<std::uint64_t>(std::min(c.gemm_n, c.npu_tile_n));
  const auto tile_k = static_cast<std::uint64_t>(std::min(c.gemm_k, c.npu_tile_k));
  const auto working_set = (tile_m * tile_k + tile_k * tile_n) * c.npu_element_bytes + tile_m * tile_n * c.npu_accumulator_bytes;
  if (c.sram_capacity_bytes < working_set) throw std::runtime_error("SRAM capacity cannot hold the configured NPU tile working set (requires " + std::to_string(working_set) + " bytes)");
  const auto unique_weight_bytes = static_cast<std::uint64_t>(c.gemm_k) * c.gemm_n * c.npu_element_bytes;
  if ((c.npu_weights_resident != 0 && c.npu_weights_resident != 1) || (c.dma_double_buffering != 0 && c.dma_double_buffering != 1)) throw std::runtime_error("Boolean architecture settings must be 0 or 1");
  if (c.npu_weights_resident == 1 && c.weight_sram_capacity_bytes < unique_weight_bytes) throw std::runtime_error("Weight SRAM cannot hold the configured NPU weight tensor (requires " + std::to_string(unique_weight_bytes) + " bytes)");
  if (c.vector_op_pj < 0 || c.npu_mac_pj < 0 || c.npu_arithmetic_energy_scale <= 0 || c.arithmetic_voltage_ratio <= 0 || c.dram_byte_pj < 0 || c.dma_byte_pj < 0) throw std::runtime_error("Energy values must be non-negative (scale and voltage ratio must be positive)");
  if (c.static_power_w < 0 || c.npu_static_power_per_mac_w < 0 || c.rvv_static_power_per_lane_w < 0 || c.sram_static_power_per_kib_w < 0 || c.dram_interface_static_power_w < 0 || c.it_power_budget_w < 0 || c.pue < 1.0 || c.electricity_cost_per_kwh < 0 || c.deployment_xpu_count < 1 || c.deployment_hours < 0) throw std::runtime_error("Power and deployment parameters are invalid");
  if (c.base_area_mm2 < 0 || c.npu_area_per_mac_mm2 < 0 || c.rvv_area_per_lane_mm2 < 0 || c.sram_area_per_kib_mm2 < 0 || c.max_area_mm2 < 0 || c.thermal_resistance_c_per_w < 0 || c.thermal_capacitance_j_per_c < 0 || c.reliability_fit_rate < 0 || c.reliability_voltage_fit_multiplier <= 0) throw std::runtime_error("Area, thermal, and reliability proxy parameters are invalid");
  if (c.frame_arrival_mode == "trace") (void)load_frame_arrivals(c);
}

[[nodiscard]] std::vector<double> load_frame_arrivals(const Config& config) {
  std::vector<double> releases;
  releases.reserve(static_cast<std::size_t>(config.frames));
  if (config.frame_arrival_mode == "periodic") {
    for (int frame = 0; frame < config.frames; ++frame) releases.push_back(frame * config.frame_period_us);
    return releases;
  }

  std::ifstream provenance(config.frame_arrival_provenance);
  if (!provenance) throw std::runtime_error("Unable to open frame-arrival provenance: " + config.frame_arrival_provenance);
  if (provenance.peek() == std::ifstream::traits_type::eof()) throw std::runtime_error("Frame-arrival provenance must not be empty");
  std::ifstream input(config.frame_arrival_trace);
  if (!input) throw std::runtime_error("Unable to open frame-arrival trace: " + config.frame_arrival_trace);
  std::string header;
  if (!std::getline(input, header) || trim(header) != "frame,release_us") {
    throw std::runtime_error("Frame-arrival trace header must be exactly: frame,release_us");
  }
  std::string line;
  int expected_frame = 0;
  double previous_release = -1.0;
  while (std::getline(input, line)) {
    if (trim(line).empty() || trim(line).starts_with('#')) continue;
    const auto comma = line.find(',');
    if (comma == std::string::npos || line.find(',', comma + 1) != std::string::npos) {
      throw std::runtime_error("Frame-arrival trace rows must contain frame,release_us");
    }
    int frame = -1;
    double release = -1.0;
    try {
      const std::string frame_text = trim(line.substr(0, comma));
      const std::string release_text = trim(line.substr(comma + 1));
      std::size_t frame_consumed = 0;
      std::size_t release_consumed = 0;
      frame = std::stoi(frame_text, &frame_consumed);
      release = std::stod(release_text, &release_consumed);
      if (frame_consumed != frame_text.size() || release_consumed != release_text.size()) throw std::invalid_argument("trailing data");
    } catch (const std::exception&) {
      throw std::runtime_error("Invalid frame-arrival trace row: " + line);
    }
    if (frame != expected_frame) throw std::runtime_error("Frame-arrival trace must contain exactly one ordered row for every frame");
    if (!std::isfinite(release) || release < 0.0 || release < previous_release) {
      throw std::runtime_error("Frame-arrival release times must be finite, nonnegative, and monotonic");
    }
    releases.push_back(release);
    previous_release = release;
    ++expected_frame;
  }
  if (expected_frame != config.frames) throw std::runtime_error("Frame-arrival trace row count must equal configured frames");
  return releases;
}

}  // namespace axissim
