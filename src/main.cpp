#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"

namespace axissim {

[[nodiscard]] std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

[[nodiscard]] std::map<std::string, std::string> read_key_values_impl(const std::filesystem::path& path, std::set<std::string>& active_paths) {
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
    if (separator == std::string::npos) {
      throw std::runtime_error("Expected key = value at " + path.string() + ":" + std::to_string(line_number));
    }
    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));
    if (key.empty() || value.empty()) {
      throw std::runtime_error("Invalid key/value at " + path.string() + ":" + std::to_string(line_number));
    }
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

[[nodiscard]] const std::set<std::string>& supported_keys() {
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

[[nodiscard]] Config load_config(const std::string& path, const std::map<std::string, std::string>& overrides) {
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

[[nodiscard]] std::vector<double> load_frame_arrivals(const Config& config);

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

struct ProfileOperator {
  std::string id;
  std::string mapping_class;
  std::vector<std::string> dependencies;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::uint64_t macs = 0;
  std::uint64_t vector_elements = 0;
  int ops_per_element = 0;
  std::uint64_t activation_read_bytes = 0;
  std::uint64_t activation_write_bytes = 0;
  std::uint64_t weight_bytes = 0;
  std::uint64_t transfer_bytes = 0;
};

struct ProfileTransferLedgerEntry {
  std::string operator_id;
  std::string category;
  std::uint64_t bytes = 0;
};

struct WorkloadProfile {
  std::string source_url;
  std::string license;
  std::string sha256;
  std::string mapping_plan_id = "implicit-profile-mapping";
  std::string mapping_rule_version = "implicit";
  std::uint64_t fused_intermediate_bytes_avoided = 0;
  int fusion_count = 0;
  std::vector<ProfileOperator> operators;
};

struct MappingPlanEntry {
  std::string operator_id;
  std::string mapping_class;
};

struct MappingFusion {
  std::string producer_id;
  std::string consumer_id;
  std::string tensor;
};

struct MappingPlan {
  std::string id;
  std::string source_model_sha256;
  std::string mapping_rule_version;
  std::vector<MappingPlanEntry> entries;
  std::vector<MappingFusion> fusions;
};

[[nodiscard]] std::optional<std::string> json_string_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) return std::nullopt;
  return match[1].str();
}

[[nodiscard]] std::optional<std::uint64_t> json_unsigned_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) return std::nullopt;
  try {
    return static_cast<std::uint64_t>(std::stoull(match[1].str()));
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid unsigned JSON field: " + key);
  }
}

[[nodiscard]] std::vector<std::string> json_string_array_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*\\[([^\\]]*)\\]");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) throw std::runtime_error("Profile operator missing array field: " + key);
  const std::string contents = trim(match[1].str());
  if (contents.empty()) return {};
  std::vector<std::string> values;
  const std::regex item("\\\"([^\\\"]+)\\\"");
  for (std::sregex_iterator iterator(contents.begin(), contents.end(), item), end; iterator != end; ++iterator) values.push_back((*iterator)[1].str());
  if (values.empty()) throw std::runtime_error("Profile array field has invalid JSON strings: " + key);
  return values;
}

[[nodiscard]] std::vector<std::string> json_array_objects(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  const auto key_position = json.find(marker);
  if (key_position == std::string::npos) throw std::runtime_error("Profile is missing array: " + key);
  const auto array_start = json.find('[', key_position + marker.size());
  if (array_start == std::string::npos) throw std::runtime_error("Profile array is malformed: " + key);
  std::vector<std::string> objects;
  int array_depth = 0;
  int object_depth = 0;
  std::size_t object_start = std::string::npos;
  for (std::size_t index = array_start; index < json.size(); ++index) {
    const char character = json[index];
    if (character == '[') ++array_depth;
    if (character == ']') {
      --array_depth;
      if (array_depth == 0) break;
    }
    if (array_depth <= 0) continue;
    if (character == '{') {
      if (object_depth++ == 0) object_start = index;
    } else if (character == '}') {
      if (--object_depth < 0) throw std::runtime_error("Profile object nesting is malformed");
      if (object_depth == 0 && object_start != std::string::npos) objects.push_back(json.substr(object_start, index - object_start + 1));
    }
  }
  if (array_depth != 0 || object_depth != 0 || objects.empty()) throw std::runtime_error("Profile operators array is malformed or empty");
  return objects;
}

[[nodiscard]] WorkloadProfile load_workload_profile(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Unable to open workload profile: " + path);
  const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (json_string_field(json, "schema_version") != std::optional<std::string>("axissim.workload-profile.v1")) {
    throw std::runtime_error("Unsupported workload-profile schema; expected axissim.workload-profile.v1");
  }
  WorkloadProfile profile;
  profile.source_url = json_string_field(json, "source_url").value_or("");
  profile.license = json_string_field(json, "license").value_or("");
  profile.sha256 = json_string_field(json, "model_sha256").value_or("");
  if (profile.source_url.empty() || profile.license.empty() || profile.sha256.size() != 64) {
    throw std::runtime_error("Workload profile source URL, license, and 64-character SHA-256 are required");
  }
  const auto objects = json_array_objects(json, "operators");
  std::set<std::string> ids;
  for (const auto& object : objects) {
    ProfileOperator operation;
    operation.id = json_string_field(object, "id").value_or("");
    operation.mapping_class = json_string_field(object, "mapping_class").value_or("");
    operation.dependencies = json_string_array_field(object, "dependencies");
    operation.inputs = json_string_array_field(object, "inputs");
    operation.outputs = json_string_array_field(object, "outputs");
    operation.macs = json_unsigned_field(object, "macs").value_or(0);
    operation.vector_elements = json_unsigned_field(object, "vector_elements").value_or(0);
    operation.ops_per_element = static_cast<int>(json_unsigned_field(object, "ops_per_element").value_or(0));
    operation.activation_read_bytes = json_unsigned_field(object, "activation_read_bytes").value_or(0);
    operation.activation_write_bytes = json_unsigned_field(object, "activation_write_bytes").value_or(0);
    operation.weight_bytes = json_unsigned_field(object, "weight_bytes").value_or(0);
    operation.transfer_bytes = operation.activation_read_bytes + operation.activation_write_bytes + operation.weight_bytes;
    if (operation.id.empty() || !ids.insert(operation.id).second) throw std::runtime_error("Profile operator IDs must be non-empty and unique");
    if (operation.mapping_class == "npu_compute" && operation.macs == 0) throw std::runtime_error("NPU profile operator requires positive MAC count: " + operation.id);
    if (operation.mapping_class == "rvv_elementwise" && (operation.vector_elements == 0 || operation.ops_per_element <= 0)) throw std::runtime_error("RVV profile operator requires positive vector geometry: " + operation.id);
    if (operation.mapping_class != "npu_compute" && operation.mapping_class != "rvv_elementwise") throw std::runtime_error("Unsupported profile mapping class: " + operation.mapping_class);
    profile.operators.push_back(std::move(operation));
  }
  std::map<std::string, int> indegree;
  std::map<std::string, std::vector<std::string>> successors;
  std::map<std::string, std::string> producer;
  for (const auto& operation : profile.operators) indegree.emplace(operation.id, 0);
  for (const auto& operation : profile.operators) {
    if (operation.outputs.empty()) throw std::runtime_error("Profile operator must name at least one output: " + operation.id);
    for (const auto& output : operation.outputs) {
      if (output.empty() || !producer.emplace(output, operation.id).second) throw std::runtime_error("Profile tensor outputs must have one non-empty producer: " + output);
    }
  }
  for (const auto& operation : profile.operators) {
    for (const auto& dependency : operation.dependencies) {
      if (!ids.contains(dependency)) throw std::runtime_error("Profile dependency does not name an operator: " + dependency);
      ++indegree.at(operation.id);
      successors[dependency].push_back(operation.id);
    }
    for (const auto& input_name : operation.inputs) {
      if (const auto producer_it = producer.find(input_name); producer_it != producer.end() &&
          std::find(operation.dependencies.cbegin(), operation.dependencies.cend(), producer_it->second) == operation.dependencies.cend()) {
        throw std::runtime_error("Profile dependency list omits producer " + producer_it->second + " for input tensor " + input_name);
      }
    }
  }
  std::queue<std::string> ready;
  for (const auto& [id, count] : indegree) if (count == 0) ready.push(id);
  int visited = 0;
  while (!ready.empty()) {
    const auto id = ready.front(); ready.pop(); ++visited;
    for (const auto& successor : successors[id]) if (--indegree.at(successor) == 0) ready.push(successor);
  }
  if (visited != static_cast<int>(profile.operators.size())) throw std::runtime_error("Workload profile dependency graph contains a cycle");
  return profile;
}

[[nodiscard]] MappingPlan load_mapping_plan(const std::string& path, const WorkloadProfile& profile) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Unable to open mapping plan: " + path);
  const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (json_string_field(json, "schema_version") != std::optional<std::string>("axissim.mapping-plan.v1")) {
    throw std::runtime_error("Unsupported mapping-plan schema; expected axissim.mapping-plan.v1");
  }
  MappingPlan plan;
  plan.id = json_string_field(json, "id").value_or("");
  plan.source_model_sha256 = json_string_field(json, "source_model_sha256").value_or("");
  plan.mapping_rule_version = json_string_field(json, "mapping_rule_version").value_or("");
  if (plan.id.empty() || plan.mapping_rule_version.empty() || plan.source_model_sha256 != profile.sha256) {
    throw std::runtime_error("Mapping plan requires id/rule version and an exact workload source-model SHA-256 match");
  }
  const auto objects = json_array_objects(json, "entries");
  std::set<std::string> mapped_ids;
  for (const auto& object : objects) {
    MappingPlanEntry entry;
    entry.operator_id = json_string_field(object, "operator_id").value_or("");
    entry.mapping_class = json_string_field(object, "mapping_class").value_or("");
    if (entry.operator_id.empty() || !mapped_ids.insert(entry.operator_id).second) {
      throw std::runtime_error("Mapping plan operator IDs must be non-empty and unique");
    }
    if (entry.mapping_class != "npu_compute" && entry.mapping_class != "rvv_elementwise") {
      throw std::runtime_error("Unsupported mapping-plan placement class: " + entry.mapping_class);
    }
    plan.entries.push_back(std::move(entry));
  }
  if (plan.entries.size() != profile.operators.size()) {
    throw std::runtime_error("Mapping plan must account for every workload-profile operator exactly once");
  }
  if (json.find("\"fusions\"") != std::string::npos) {
    std::set<std::string> participating_operators;
    const auto fusion_objects = json_array_objects(json, "fusions");
    std::map<std::string, const ProfileOperator*> operators_by_id;
    for (const auto& operation : profile.operators) operators_by_id.emplace(operation.id, &operation);
    for (const auto& object : fusion_objects) {
      MappingFusion fusion;
      fusion.producer_id = json_string_field(object, "producer_id").value_or("");
      fusion.consumer_id = json_string_field(object, "consumer_id").value_or("");
      fusion.tensor = json_string_field(object, "tensor").value_or("");
      const auto producer = operators_by_id.find(fusion.producer_id);
      const auto consumer = operators_by_id.find(fusion.consumer_id);
      if (fusion.producer_id.empty() || fusion.consumer_id.empty() || fusion.tensor.empty() || producer == operators_by_id.end() || consumer == operators_by_id.end()) {
        throw std::runtime_error("Fusion must name existing producer, consumer, and tensor IDs");
      }
      if (!participating_operators.insert(fusion.producer_id).second || !participating_operators.insert(fusion.consumer_id).second) {
        throw std::runtime_error("V1 fusion plans cannot overlap operators");
      }
      const auto& producer_op = *producer->second;
      const auto& consumer_op = *consumer->second;
      if (producer_op.outputs.size() != 1 || producer_op.outputs.front() != fusion.tensor ||
          std::find(consumer_op.inputs.cbegin(), consumer_op.inputs.cend(), fusion.tensor) == consumer_op.inputs.cend() ||
          std::find(consumer_op.dependencies.cbegin(), consumer_op.dependencies.cend(), fusion.producer_id) == consumer_op.dependencies.cend()) {
        throw std::runtime_error("Fusion must be a direct declared producer-consumer tensor edge");
      }
      const int consumers = static_cast<int>(std::count_if(profile.operators.cbegin(), profile.operators.cend(), [&fusion](const ProfileOperator& operation) {
        return std::find(operation.inputs.cbegin(), operation.inputs.cend(), fusion.tensor) != operation.inputs.cend();
      }));
      if (consumers != 1) throw std::runtime_error("V1 fusion requires a single-consumer intermediate tensor");
      if (producer_op.activation_write_bytes == 0 || producer_op.activation_write_bytes > consumer_op.activation_read_bytes) {
        throw std::runtime_error("Fusion tensor bytes cannot be reconciled with declared activation traffic");
      }
      plan.fusions.push_back(std::move(fusion));
    }
  }
  return plan;
}

void apply_mapping_plan(WorkloadProfile& profile, const MappingPlan& plan) {
  std::map<std::string, std::string> mapping_by_id;
  for (const auto& entry : plan.entries) mapping_by_id.emplace(entry.operator_id, entry.mapping_class);
  for (auto& operation : profile.operators) {
    const auto it = mapping_by_id.find(operation.id);
    if (it == mapping_by_id.end()) throw std::runtime_error("Mapping plan omits workload operator: " + operation.id);
    // V1 makes the current implicit placement explicit. Cross-engine
    // placement becomes legal only once each profile records a valid cost
    // model for both resources; never invent a zero-cost alternate mapping.
    if (it->second != operation.mapping_class) {
      throw std::runtime_error("Mapping-plan placement is unsupported for operator " + operation.id + " in v1; provide the baseline class");
    }
  }
  std::map<std::string, ProfileOperator*> operators_by_id;
  for (auto& operation : profile.operators) operators_by_id.emplace(operation.id, &operation);
  for (const auto& fusion : plan.fusions) {
    auto& producer = *operators_by_id.at(fusion.producer_id);
    auto& consumer = *operators_by_id.at(fusion.consumer_id);
    const auto bytes = producer.activation_write_bytes;
    producer.activation_write_bytes -= bytes;
    consumer.activation_read_bytes -= bytes;
    producer.transfer_bytes = producer.activation_read_bytes + producer.activation_write_bytes + producer.weight_bytes;
    consumer.transfer_bytes = consumer.activation_read_bytes + consumer.activation_write_bytes + consumer.weight_bytes;
    profile.fused_intermediate_bytes_avoided += 2 * bytes;
    ++profile.fusion_count;
  }
  profile.mapping_plan_id = plan.id;
  profile.mapping_rule_version = plan.mapping_rule_version;
}

struct TraceEvent { int frame; std::string task; std::string resource; double start_us; double end_us; };

class Simulator {
 public:
  explicit Simulator(Config config) : c_(std::move(config)) {}

  void run() {
    frame_arrivals_us_ = load_frame_arrivals(c_);
    if (c_.workload_mode == "profiled_graph") {
      profile_ = load_workload_profile(c_.workload_profile);
      if (!c_.mapping_plan.empty()) apply_mapping_plan(profile_, load_mapping_plan(c_.mapping_plan, profile_));
      run_profile_scheduler();
    } else {
      run_event_scheduler();
    }
  }

  [[nodiscard]] const std::vector<TraceEvent>& trace() const { return trace_; }
  [[nodiscard]] const std::vector<double>& latencies_us() const { return latencies_us_; }
  [[nodiscard]] int deadline_misses() const { return deadline_misses_; }
  [[nodiscard]] int control_tasks() const { return control_tasks_; }
  [[nodiscard]] int control_deadline_misses() const { return control_deadline_misses_; }
  [[nodiscard]] const std::vector<double>& vision_start_delays_us() const { return vision_start_delays_us_; }
  [[nodiscard]] const std::vector<double>& vision_resource_queue_delays_us() const { return vision_resource_queue_delays_us_; }
  [[nodiscard]] const std::vector<double>& vision_completion_times_us() const { return vision_completion_times_us_; }
  [[nodiscard]] const std::vector<double>& control_response_times_us() const { return control_response_times_us_; }
  [[nodiscard]] int max_pending_tasks() const { return max_pending_tasks_; }
  [[nodiscard]] const std::vector<ProfileTransferLedgerEntry>& profile_transfer_ledger() const { return profile_transfer_ledger_; }
  [[nodiscard]] std::uint64_t profile_fused_intermediate_bytes_avoided() const { return profile_.fused_intermediate_bytes_avoided; }
  [[nodiscard]] int profile_fusion_count() const { return profile_.fusion_count; }
  [[nodiscard]] double profile_vector_ops_per_frame() const {
    return std::accumulate(profile_.operators.cbegin(), profile_.operators.cend(), 0.0,
                           [](double total, const ProfileOperator& operation) { return total + static_cast<double>(operation.vector_elements) * operation.ops_per_element; });
  }
  [[nodiscard]] double profile_npu_macs_per_frame() const {
    return std::accumulate(profile_.operators.cbegin(), profile_.operators.cend(), 0.0,
                           [](double total, const ProfileOperator& operation) { return total + static_cast<double>(operation.macs); });
  }
  [[nodiscard]] double total_energy_pj() const { return total_energy_pj_; }
  [[nodiscard]] double vector_energy_pj() const { return vector_energy_pj_; }
  [[nodiscard]] double npu_arithmetic_energy_pj() const { return npu_arithmetic_energy_pj_; }
  [[nodiscard]] double dram_energy_pj() const { return dram_energy_pj_; }
  [[nodiscard]] double dma_energy_pj() const { return dma_energy_pj_; }
  [[nodiscard]] std::uint64_t total_dram_bytes() const { return total_dram_bytes_; }
  [[nodiscard]] int vlmax() const { return (c_.rvv_vlen_bits / c_.rvv_sew_bits) * c_.rvv_lmul; }
  [[nodiscard]] double rvv_stream_compute_cycles() const {
    if (c_.workload_mode != "rvv_stream") throw std::runtime_error("RVV stream cycle reporting requires workload_mode=rvv_stream");
    return vector_cycles(c_.preprocess_elements, c_.preprocess_ops_per_element);
  }
  [[nodiscard]] double tail_utilization(std::uint64_t elements) const {
    const auto iterations = (elements + static_cast<std::uint64_t>(vlmax()) - 1) / static_cast<std::uint64_t>(vlmax());
    return static_cast<double>(elements) / static_cast<double>(iterations * static_cast<std::uint64_t>(vlmax()));
  }
  [[nodiscard]] std::uint64_t npu_tile_working_set_bytes() const {
    const auto tile_m = static_cast<std::uint64_t>(std::min(c_.gemm_m, c_.npu_tile_m));
    const auto tile_n = static_cast<std::uint64_t>(std::min(c_.gemm_n, c_.npu_tile_n));
    const auto tile_k = static_cast<std::uint64_t>(std::min(c_.gemm_k, c_.npu_tile_k));
    return (tile_m * tile_k + tile_k * tile_n) * c_.npu_element_bytes + tile_m * tile_n * c_.npu_accumulator_bytes;
  }
  [[nodiscard]] std::uint64_t unique_weight_bytes() const {
    return static_cast<std::uint64_t>(c_.gemm_k) * c_.gemm_n * c_.npu_element_bytes;
  }
  [[nodiscard]] std::uint64_t npu_weight_tile_bytes_per_frame() const {
    std::uint64_t bytes = 0;
    for (int m0 = 0; m0 < c_.gemm_m; m0 += c_.npu_tile_m) {
      for (int n0 = 0; n0 < c_.gemm_n; n0 += c_.npu_tile_n) {
        const auto n = static_cast<std::uint64_t>(std::min(c_.npu_tile_n, c_.gemm_n - n0));
        for (int k0 = 0; k0 < c_.gemm_k; k0 += c_.npu_tile_k) {
          const auto k = static_cast<std::uint64_t>(std::min(c_.npu_tile_k, c_.gemm_k - k0));
          bytes += k * n * c_.npu_element_bytes;
        }
      }
    }
    return bytes;
  }
  [[nodiscard]] std::uint64_t npu_activation_output_bytes_per_frame() const {
    std::uint64_t bytes = 0;
    for (int m0 = 0; m0 < c_.gemm_m; m0 += c_.npu_tile_m) {
      const auto m = static_cast<std::uint64_t>(std::min(c_.npu_tile_m, c_.gemm_m - m0));
      for (int n0 = 0; n0 < c_.gemm_n; n0 += c_.npu_tile_n) {
        const auto n = static_cast<std::uint64_t>(std::min(c_.npu_tile_n, c_.gemm_n - n0));
        for (int k0 = 0; k0 < c_.gemm_k; k0 += c_.npu_tile_k) {
          const auto k = static_cast<std::uint64_t>(std::min(c_.npu_tile_k, c_.gemm_k - k0));
          bytes += m * k * c_.npu_element_bytes;
        }
        bytes += m * n * c_.npu_accumulator_bytes;
      }
    }
    return bytes;
  }
  [[nodiscard]] std::uint64_t npu_transfer_bytes(int frame) const {
    const auto weights = c_.npu_weights_resident == 1 ? (frame == 0 ? unique_weight_bytes() : 0) : npu_weight_tile_bytes_per_frame();
    return npu_activation_output_bytes_per_frame() + weights;
  }
  [[nodiscard]] std::uint64_t npu_steady_state_dram_bytes_per_frame() const {
    return npu_transfer_bytes(c_.npu_weights_resident == 1 ? 1 : 0);
  }
  [[nodiscard]] double makespan_us() const {
    if (trace_.empty()) return 0.0;
    double end = 0.0;
    for (const auto& event : trace_) end = std::max(end, event.end_us);
    return end;
  }
  [[nodiscard]] double resource_busy_us(const std::string& resource) const {
    double busy = 0.0;
    for (const auto& event : trace_) {
      if (event.resource == resource) busy += event.end_us - event.start_us;
    }
    return busy;
  }
  [[nodiscard]] int max_concurrent_tasks() const {
    std::vector<std::pair<double, int>> boundaries;
    boundaries.reserve(trace_.size() * 2);
    for (const auto& event : trace_) {
      boundaries.emplace_back(event.start_us, 1);
      boundaries.emplace_back(event.end_us, -1);
    }
    std::sort(boundaries.begin(), boundaries.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) return lhs.first < rhs.first;
      return lhs.second < rhs.second;  // End a task before starting one at the same timestamp.
    });
    int active = 0;
    int maximum = 0;
    for (const auto& boundary : boundaries) {
      active += boundary.second;
      maximum = std::max(maximum, active);
    }
    return maximum;
  }

 private:
  enum class RuntimeResource : std::size_t { Dma = 0, Rvv = 1, Npu = 2 };
  enum class RuntimeStage : int { Camera = 0, PreRead, Preprocess, PreWrite, NpuData, NpuCompute, PostRead, Postprocess, Control, VectorKernel, ProfileTransfer, ProfileCompute };

  struct RuntimeTask {
    int frame = 0;
    RuntimeStage stage = RuntimeStage::Camera;
    RuntimeResource resource = RuntimeResource::Dma;
    double release_us = 0.0;
    double ready_us = 0.0;
    double duration_us = 0.0;
    double vector_energy_pj = 0.0;
    double npu_energy_pj = 0.0;
    std::uint64_t transfer_bytes = 0;
    std::uint64_t vector_offset = 0;
    std::uint64_t vector_elements = 0;
    int priority = 1;
    std::string name;
    int profile_index = -1;
  };

  struct FutureTaskOrder {
    bool operator()(const RuntimeTask& left, const RuntimeTask& right) const { return left.ready_us > right.ready_us; }
  };
  struct Completion { double end_us; RuntimeResource resource; RuntimeTask task; };
  struct CompletionOrder {
    bool operator()(const Completion& left, const Completion& right) const { return left.end_us > right.end_us; }
  };

  [[nodiscard]] std::uint64_t vector_chunk_size(std::uint64_t total_elements, std::uint64_t offset) const {
    const auto remaining = total_elements - offset;
    return c_.rvv_chunk_elements == 0 ? remaining : std::min(remaining, c_.rvv_chunk_elements);
  }

  [[nodiscard]] std::uint64_t vector_transfer_bytes(std::uint64_t elements) const {
    return c_.rvv_memory_accounting == "extra_traffic" ? elements * static_cast<std::uint64_t>(c_.rvv_memory_bytes_per_element) : 0;
  }

  [[nodiscard]] RuntimeTask make_runtime_task(int frame, RuntimeStage stage, double release_us, double ready_us, std::uint64_t vector_offset = 0) const {
    RuntimeTask task;
    task.frame = frame;
    task.stage = stage;
    task.release_us = release_us;
    task.ready_us = ready_us;
    switch (stage) {
      case RuntimeStage::Camera: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.input_bytes), 0.0, 0.0, c_.input_bytes, 0, 0, 1, "camera_ingest"}; break;
      case RuntimeStage::PreRead: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.input_bytes), 0.0, 0.0, c_.input_bytes, 0, 0, 1, "preprocess_read"}; break;
      case RuntimeStage::Preprocess: {
        const auto elements = vector_chunk_size(c_.preprocess_elements, vector_offset);
        task = {frame, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(elements, c_.preprocess_ops_per_element), static_cast<double>(elements) * c_.preprocess_ops_per_element * c_.vector_op_pj, 0.0, vector_transfer_bytes(elements), vector_offset, elements, 1, "rvv_preprocess"};
        break;
      }
      case RuntimeStage::PreWrite: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.preprocess_output_bytes), 0.0, 0.0, c_.preprocess_output_bytes, 0, 0, 1, "preprocess_write"}; break;
      case RuntimeStage::NpuData: {
        const auto bytes = npu_transfer_bytes(frame);
        task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(bytes), 0.0, 0.0, bytes, 0, 0, 1, "npu_tiled_operands"};
        break;
      }
      case RuntimeStage::NpuCompute: {
        const double macs = static_cast<double>(c_.gemm_m) * c_.gemm_n * c_.gemm_k;
        task = {frame, stage, RuntimeResource::Npu, release_us, ready_us, std::ceil(macs / c_.npu_macs) / (c_.npu_frequency_ghz * 1000.0), 0.0, macs * c_.npu_mac_pj * c_.npu_arithmetic_energy_scale * c_.arithmetic_voltage_ratio * c_.arithmetic_voltage_ratio, 0, 0, 0, 1, "npu_gemm"};
        break;
      }
      case RuntimeStage::PostRead: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.postprocess_bytes), 0.0, 0.0, c_.postprocess_bytes, 0, 0, 1, "postprocess_read"}; break;
      case RuntimeStage::Postprocess: {
        const auto elements = vector_chunk_size(c_.postprocess_elements, vector_offset);
        task = {frame, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(elements, c_.postprocess_ops_per_element), static_cast<double>(elements) * c_.postprocess_ops_per_element * c_.vector_op_pj, 0.0, vector_transfer_bytes(elements), vector_offset, elements, 1, "rvv_postprocess"};
        break;
      }
      case RuntimeStage::Control: task = {-1, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(c_.control_ops, 1), static_cast<double>(c_.control_ops) * c_.vector_op_pj, 0.0, 0, 0, 0, 0, "safety_control"}; break;
      case RuntimeStage::VectorKernel: {
        const auto elements = vector_chunk_size(c_.preprocess_elements, vector_offset);
        task = {frame, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(elements, c_.preprocess_ops_per_element), static_cast<double>(elements) * c_.preprocess_ops_per_element * c_.vector_op_pj, 0.0, vector_transfer_bytes(elements), vector_offset, elements, 1, "rvv_stream_kernel"};
        break;
      }
      case RuntimeStage::ProfileTransfer:
      case RuntimeStage::ProfileCompute: throw std::runtime_error("Profile tasks are constructed by the profile scheduler");
    }
    return task;
  }

  [[nodiscard]] double transfer_duration_us(std::uint64_t bytes) const {
    return c_.dma_setup_us + static_cast<double>(bytes) / c_.dram_bandwidth_gbps / 1000.0;
  }

  void run_profile_scheduler() {
    const auto& operations = profile_.operators;
    profile_transfer_ledger_.clear();
    for (const auto& operation : operations) {
      const auto append_transfer = [this, &operation](const std::string& category, std::uint64_t bytes) {
        if (bytes > 0) profile_transfer_ledger_.push_back({operation.id, category, bytes});
      };
      append_transfer("activation_read", operation.activation_read_bytes);
      append_transfer("activation_write", operation.activation_write_bytes);
      append_transfer("weight_read", operation.weight_bytes);
    }
    const auto ledger_bytes = std::accumulate(profile_transfer_ledger_.cbegin(), profile_transfer_ledger_.cend(), std::uint64_t{0},
                                              [](std::uint64_t total, const ProfileTransferLedgerEntry& entry) { return total + entry.bytes; });
    const auto profile_bytes = std::accumulate(operations.cbegin(), operations.cend(), std::uint64_t{0},
                                               [](std::uint64_t total, const ProfileOperator& operation) { return total + operation.transfer_bytes; });
    if (ledger_bytes != profile_bytes) throw std::runtime_error("Profile transfer ledger does not reconcile with operator transfer totals");
    std::map<std::string, int> index_for_id;
    std::vector<std::vector<int>> successors(operations.size());
    std::vector<int> base_dependencies(operations.size(), 0);
    for (std::size_t index = 0; index < operations.size(); ++index) index_for_id.emplace(operations[index].id, static_cast<int>(index));
    for (std::size_t index = 0; index < operations.size(); ++index) {
      base_dependencies[index] = static_cast<int>(operations[index].dependencies.size());
      for (const auto& dependency : operations[index].dependencies) successors[static_cast<std::size_t>(index_for_id.at(dependency))].push_back(static_cast<int>(index));
    }
    std::vector<int> roots;
    int terminals_per_frame = 0;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (base_dependencies[index] == 0) roots.push_back(static_cast<int>(index));
      if (successors[index].empty()) ++terminals_per_frame;
    }
    std::priority_queue<RuntimeTask, std::vector<RuntimeTask>, FutureTaskOrder> future;
    std::priority_queue<Completion, std::vector<Completion>, CompletionOrder> completions;
    std::array<std::vector<RuntimeTask>, 3> pending;
    std::array<bool, 3> busy = {false, false, false};
    std::vector<std::vector<int>> remaining(static_cast<std::size_t>(c_.frames), base_dependencies);
    std::vector<int> remaining_terminals(static_cast<std::size_t>(c_.frames), terminals_per_frame);
    std::vector<double> terminal_end(static_cast<std::size_t>(c_.frames), 0.0);

    const auto make_profile_task = [this, &operations](int frame, int profile_index, bool transfer, double release, double ready) {
      const auto& operation = operations[static_cast<std::size_t>(profile_index)];
      RuntimeTask task;
      task.frame = frame;
      task.stage = transfer ? RuntimeStage::ProfileTransfer : RuntimeStage::ProfileCompute;
      task.release_us = release;
      task.ready_us = ready;
      task.profile_index = profile_index;
      if (transfer) {
        task.resource = RuntimeResource::Dma;
        task.duration_us = transfer_duration_us(operation.transfer_bytes);
        task.transfer_bytes = operation.transfer_bytes;
        task.name = "profile_dma:" + operation.id;
      } else if (operation.mapping_class == "npu_compute") {
        task.resource = RuntimeResource::Npu;
        task.duration_us = std::ceil(static_cast<double>(operation.macs) / c_.npu_macs) / (c_.npu_frequency_ghz * 1000.0);
        task.npu_energy_pj = static_cast<double>(operation.macs) * c_.npu_mac_pj * c_.npu_arithmetic_energy_scale * c_.arithmetic_voltage_ratio * c_.arithmetic_voltage_ratio;
        task.name = "profile_npu:" + operation.id;
      } else {
        task.resource = RuntimeResource::Rvv;
        task.duration_us = vector_duration_us(operation.vector_elements, operation.ops_per_element);
        task.vector_energy_pj = static_cast<double>(operation.vector_elements) * operation.ops_per_element * c_.vector_op_pj;
        task.name = "profile_rvv:" + operation.id;
      }
      return task;
    };
    const auto enqueue_operation = [&future, &make_profile_task, &operations](int frame, int operation, double release, double ready) {
      future.push(make_profile_task(frame, operation, operations[static_cast<std::size_t>(operation)].transfer_bytes > 0, release, ready));
    };
    for (int frame = 0; frame < c_.frames; ++frame) {
      const double release = frame_arrivals_us_[static_cast<std::size_t>(frame)];
      for (const int root : roots) enqueue_operation(frame, root, release, release);
    }
    if (c_.control_period_us > 0.0) {
      const double horizon_us = frame_arrivals_us_.back() + c_.frame_period_us;
      for (double release = 0.0; release < horizon_us; release += c_.control_period_us) future.push(make_runtime_task(-1, RuntimeStage::Control, release, release));
    }
    const auto resource_name = [](RuntimeResource resource) { return resource == RuntimeResource::Dma ? "DMA+DRAM" : resource == RuntimeResource::Rvv ? "RVV" : "NPU"; };
    double time = 0.0;
    while (!future.empty() || !completions.empty()) {
      const double next_future = future.empty() ? std::numeric_limits<double>::infinity() : future.top().ready_us;
      const double next_completion = completions.empty() ? std::numeric_limits<double>::infinity() : completions.top().end_us;
      time = std::min(next_future, next_completion);
      while (!future.empty() && future.top().ready_us <= time) { RuntimeTask task = future.top(); future.pop(); pending[static_cast<std::size_t>(task.resource)].push_back(std::move(task)); }
      int pending_count = 0;
      for (const auto& queue : pending) pending_count += static_cast<int>(queue.size());
      max_pending_tasks_ = std::max(max_pending_tasks_, pending_count);
      while (!completions.empty() && completions.top().end_us <= time) {
        Completion completion = completions.top(); completions.pop();
        busy[static_cast<std::size_t>(completion.resource)] = false;
        const RuntimeTask& task = completion.task;
        if (task.stage == RuntimeStage::ProfileTransfer) {
          future.push(make_profile_task(task.frame, task.profile_index, false, task.release_us, completion.end_us));
        } else if (task.stage == RuntimeStage::ProfileCompute) {
          const auto operation = static_cast<std::size_t>(task.profile_index);
          for (const int successor : successors[operation]) {
            if (--remaining[static_cast<std::size_t>(task.frame)][static_cast<std::size_t>(successor)] == 0) {
              enqueue_operation(task.frame, successor, task.release_us, completion.end_us);
            }
          }
          if (successors[operation].empty()) {
            auto& end = terminal_end[static_cast<std::size_t>(task.frame)];
            end = std::max(end, completion.end_us);
            if (--remaining_terminals[static_cast<std::size_t>(task.frame)] == 0) {
              const double latency = end - task.release_us;
              latencies_us_.push_back(latency);
              vision_completion_times_us_.push_back(end);
              if (latency > c_.deadline_us) ++deadline_misses_;
            }
          }
        } else if (task.stage == RuntimeStage::Control) {
          ++control_tasks_;
          const double response = completion.end_us - task.release_us;
          control_response_times_us_.push_back(response);
          if (response > c_.control_deadline_us) ++control_deadline_misses_;
        }
      }
      for (std::size_t index = 0; index < pending.size(); ++index) {
        if (busy[index] || pending[index].empty()) continue;
        auto& queue = pending[index];
        const auto selected = std::min_element(queue.begin(), queue.end(), [this](const RuntimeTask& left, const RuntimeTask& right) {
          if (c_.scheduler_policy == "fixed_priority" && left.priority != right.priority) return left.priority < right.priority;
          if (c_.scheduler_policy == "edf") {
            const double left_deadline = left.release_us + (left.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
            const double right_deadline = right.release_us + (right.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
            if (left_deadline != right_deadline) return left_deadline < right_deadline;
          }
          return left.ready_us < right.ready_us;
        });
        RuntimeTask task = *selected; queue.erase(selected); busy[index] = true;
        if (task.frame >= 0) vision_resource_queue_delays_us_.push_back(time - task.ready_us);
        trace_.push_back({task.frame, task.name, resource_name(task.resource), time, time + task.duration_us});
        vector_energy_pj_ += task.vector_energy_pj;
        npu_arithmetic_energy_pj_ += task.npu_energy_pj;
        if (task.transfer_bytes > 0) { total_dram_bytes_ += task.transfer_bytes; dram_energy_pj_ += static_cast<double>(task.transfer_bytes) * c_.dram_byte_pj; dma_energy_pj_ += static_cast<double>(task.transfer_bytes) * c_.dma_byte_pj; }
        total_energy_pj_ += task.vector_energy_pj + task.npu_energy_pj + static_cast<double>(task.transfer_bytes) * (c_.dram_byte_pj + c_.dma_byte_pj);
        completions.push({time + task.duration_us, task.resource, std::move(task)});
      }
    }
  }

  void run_event_scheduler() {
    std::priority_queue<RuntimeTask, std::vector<RuntimeTask>, FutureTaskOrder> future;
    std::priority_queue<Completion, std::vector<Completion>, CompletionOrder> completions;
    std::array<std::vector<RuntimeTask>, 3> pending;
    std::array<bool, 3> busy = {false, false, false};
    const auto initial_stage = c_.workload_mode == "rvv_stream" ? RuntimeStage::VectorKernel : RuntimeStage::Camera;
    if (c_.dma_double_buffering == 1) {
      for (int frame = 0; frame < c_.frames; ++frame) future.push(make_runtime_task(frame, initial_stage, frame_arrivals_us_[static_cast<std::size_t>(frame)], frame_arrivals_us_[static_cast<std::size_t>(frame)]));
    } else {
      future.push(make_runtime_task(0, initial_stage, 0.0, 0.0));
    }
    if (c_.control_period_us > 0.0) {
      const double horizon_us = frame_arrivals_us_.back() + c_.frame_period_us;
      for (double release = 0.0; release < horizon_us; release += c_.control_period_us) future.push(make_runtime_task(-1, RuntimeStage::Control, release, release));
    }
    auto resource_name = [](RuntimeResource resource) { return resource == RuntimeResource::Dma ? "DMA+DRAM" : resource == RuntimeResource::Rvv ? "RVV" : "NPU"; };
    double time = 0.0;
    while (!future.empty() || !completions.empty()) {
      const double next_future = future.empty() ? std::numeric_limits<double>::infinity() : future.top().ready_us;
      const double next_completion = completions.empty() ? std::numeric_limits<double>::infinity() : completions.top().end_us;
      time = std::min(next_future, next_completion);
      while (!future.empty() && future.top().ready_us <= time) {
        RuntimeTask task = future.top(); future.pop();
        pending[static_cast<std::size_t>(task.resource)].push_back(std::move(task));
      }
      int pending_count = 0;
      for (const auto& queue : pending) pending_count += static_cast<int>(queue.size());
      max_pending_tasks_ = std::max(max_pending_tasks_, pending_count);
      while (!completions.empty() && completions.top().end_us <= time) {
        Completion completion = completions.top(); completions.pop();
        busy[static_cast<std::size_t>(completion.resource)] = false;
        const RuntimeTask& task = completion.task;
        const auto total_vector_elements = (task.stage == RuntimeStage::Preprocess || task.stage == RuntimeStage::VectorKernel) ? c_.preprocess_elements : task.stage == RuntimeStage::Postprocess ? c_.postprocess_elements : 0;
        if (total_vector_elements > 0 && task.vector_offset + task.vector_elements < total_vector_elements) {
          future.push(make_runtime_task(task.frame, task.stage, task.release_us, completion.end_us, task.vector_offset + task.vector_elements));
        } else if (task.stage == RuntimeStage::Postprocess || task.stage == RuntimeStage::VectorKernel) {
          const double latency = completion.end_us - task.release_us;
          latencies_us_.push_back(latency);
          vision_completion_times_us_.push_back(completion.end_us);
          if (latency > c_.deadline_us) ++deadline_misses_;
          if (c_.dma_double_buffering == 0 && task.frame + 1 < c_.frames) {
            const double next_release = frame_arrivals_us_[static_cast<std::size_t>(task.frame + 1)];
            future.push(make_runtime_task(task.frame + 1, initial_stage, next_release, std::max(next_release, completion.end_us)));
          }
        } else if (task.stage == RuntimeStage::Control) {
          ++control_tasks_;
          const double response = completion.end_us - task.release_us;
          control_response_times_us_.push_back(response);
          if (response > c_.control_deadline_us) ++control_deadline_misses_;
        } else {
          future.push(make_runtime_task(task.frame, static_cast<RuntimeStage>(static_cast<int>(task.stage) + 1), task.release_us, completion.end_us));
        }
      }
      for (std::size_t index = 0; index < pending.size(); ++index) {
        if (busy[index] || pending[index].empty()) continue;
        auto& queue = pending[index];
        const auto selected = std::min_element(queue.begin(), queue.end(), [this](const RuntimeTask& left, const RuntimeTask& right) {
          if (c_.scheduler_policy == "fixed_priority" && left.priority != right.priority) return left.priority < right.priority;
          if (c_.scheduler_policy == "edf") {
            const double left_deadline = left.release_us + (left.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
            const double right_deadline = right.release_us + (right.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
            if (left_deadline != right_deadline) return left_deadline < right_deadline;
          }
          return left.ready_us < right.ready_us;
        });
        RuntimeTask task = *selected;
        queue.erase(selected);
        busy[index] = true;
        if (task.frame >= 0) vision_resource_queue_delays_us_.push_back(time - task.ready_us);
        if ((task.stage == RuntimeStage::Camera || task.stage == RuntimeStage::VectorKernel) && task.vector_offset == 0) {
          vision_start_delays_us_.push_back(time - task.release_us);
        }
        const double end = time + task.duration_us;
        trace_.push_back({task.frame, task.name, resource_name(task.resource), time, end});
        vector_energy_pj_ += task.vector_energy_pj;
        npu_arithmetic_energy_pj_ += task.npu_energy_pj;
        if (task.transfer_bytes > 0) {
          total_dram_bytes_ += task.transfer_bytes;
          dram_energy_pj_ += static_cast<double>(task.transfer_bytes) * c_.dram_byte_pj;
          dma_energy_pj_ += static_cast<double>(task.transfer_bytes) * c_.dma_byte_pj;
        }
        total_energy_pj_ += task.vector_energy_pj + task.npu_energy_pj + static_cast<double>(task.transfer_bytes) * (c_.dram_byte_pj + c_.dma_byte_pj);
        completions.push({end, task.resource, std::move(task)});
      }
    }
  }

  [[nodiscard]] double vector_duration_us(std::uint64_t elements, int ops_per_element) const {
    return vector_cycles(elements, ops_per_element) / (c_.rvv_frequency_ghz * 1000.0);
  }

  [[nodiscard]] double vector_cycles(std::uint64_t elements, int ops_per_element) const {
    const double lane_rate = static_cast<double>(c_.rvv_lanes) * c_.rvv_ops_per_lane_cycle;
    const double compute_cycles = std::ceil(static_cast<double>(elements) * ops_per_element / lane_rate);
    const auto iterations = (elements + static_cast<std::uint64_t>(vlmax()) - 1) / static_cast<std::uint64_t>(vlmax());
    const double scaled_compute = compute_cycles * (c_.rvv_timing_model == "affine_overhead" ? c_.rvv_compute_cycle_scale : 1.0);
    return scaled_compute + iterations * c_.rvv_stripmine_overhead_cycles + (c_.rvv_timing_model == "affine_overhead" ? c_.rvv_affine_overhead_cycles : 0.0);
  }

  Config c_;
  WorkloadProfile profile_;
  std::vector<ProfileTransferLedgerEntry> profile_transfer_ledger_;
  std::vector<double> frame_arrivals_us_;
  std::vector<TraceEvent> trace_;
  std::vector<double> latencies_us_;
  std::vector<double> vision_start_delays_us_;
  std::vector<double> vision_resource_queue_delays_us_;
  std::vector<double> vision_completion_times_us_;
  std::vector<double> control_response_times_us_;
  int deadline_misses_ = 0;
  int control_tasks_ = 0;
  int control_deadline_misses_ = 0;
  int max_pending_tasks_ = 0;
  double total_energy_pj_ = 0.0;
  double vector_energy_pj_ = 0.0;
  double npu_arithmetic_energy_pj_ = 0.0;
  double dram_energy_pj_ = 0.0;
  double dma_energy_pj_ = 0.0;
  std::uint64_t total_dram_bytes_ = 0;
};

[[nodiscard]] double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  const double index = (values.size() - 1) * p;
  const auto low = static_cast<std::size_t>(std::floor(index));
  const auto high = static_cast<std::size_t>(std::ceil(index));
  return values[low] + (values[high] - values[low]) * (index - low);
}

[[nodiscard]] double max_output_interval_jitter_us(const std::vector<double>& completions, double nominal_period_us) {
  if (completions.size() < 2) return 0.0;
  double maximum = 0.0;
  for (std::size_t index = 1; index < completions.size(); ++index) {
    maximum = std::max(maximum, std::abs((completions[index] - completions[index - 1]) - nominal_period_us));
  }
  return maximum;
}

void write_trace_csv(const std::string& path, const std::vector<TraceEvent>& trace) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Unable to write trace: " + path);
  output << "frame,task,resource,start_us,end_us,duration_us\n";
  output << std::fixed << std::setprecision(3);
  for (const auto& event : trace) {
    output << event.frame << ',' << event.task << ',' << event.resource << ',' << event.start_us << ',' << event.end_us << ',' << (event.end_us - event.start_us) << '\n';
  }
}

[[nodiscard]] std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    escaped.push_back(character);
  }
  return escaped;
}

[[nodiscard]] double modeled_static_power_w(const Config& config) {
  return config.static_power_w
       + static_cast<double>(config.npu_macs) * config.npu_static_power_per_mac_w
       + static_cast<double>(config.rvv_lanes) * config.rvv_static_power_per_lane_w
       + (static_cast<double>(config.sram_capacity_bytes) / 1024.0) * config.sram_static_power_per_kib_w
       + config.dram_interface_static_power_w;
}

[[nodiscard]] double modeled_area_mm2(const Config& config) {
  return config.base_area_mm2
       + static_cast<double>(config.npu_macs) * config.npu_area_per_mac_mm2
       + static_cast<double>(config.rvv_lanes) * config.rvv_area_per_lane_mm2
       + (static_cast<double>(config.sram_capacity_bytes) / 1024.0) * config.sram_area_per_kib_mm2;
}

void write_result_json(const std::string& path, const Simulator& simulator, const Config& config) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Unable to write JSON: " + path);
  const auto& latency = simulator.latencies_us();
  const double mean = std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size();
  const double energy_per_frame_uj = simulator.total_energy_pj() / latency.size() / 1'000'000.0;
  const double static_power_w = modeled_static_power_w(config);
  const double static_energy_pj = static_power_w * simulator.makespan_us() * 1'000'000.0;
  const double it_energy_pj = simulator.total_energy_pj() + static_energy_pj;
  const double average_it_power_w = simulator.makespan_us() > 0 ? it_energy_pj / simulator.makespan_us() / 1'000'000.0 : 0.0;
  const double deployment_facility_kwh = config.pue * average_it_power_w * config.deployment_xpu_count * config.deployment_hours / 1000.0;
  const double area_mm2 = modeled_area_mm2(config);
  const double steady_temp_c = config.ambient_temp_c + average_it_power_w * config.thermal_resistance_c_per_w;
  const double tau_s = config.thermal_resistance_c_per_w * config.thermal_capacitance_j_per_c;
  const double end_temp_c = tau_s > 0 ? config.ambient_temp_c + (steady_temp_c - config.ambient_temp_c) * (1.0 - std::exp(-(simulator.makespan_us() / 1'000'000.0) / tau_s)) : steady_temp_c;
  const double expected_failures = config.reliability_fit_rate * config.reliability_voltage_fit_multiplier * config.deployment_xpu_count * config.deployment_hours / 1'000'000'000.0;
  const auto& queue_delay = simulator.vision_resource_queue_delays_us();
  const auto& control_response = simulator.control_response_times_us();
  const double queue_delay_mean = queue_delay.empty() ? 0.0 : std::accumulate(queue_delay.begin(), queue_delay.end(), 0.0) / queue_delay.size();
  const double queue_delay_max = queue_delay.empty() ? 0.0 : *std::max_element(queue_delay.begin(), queue_delay.end());
  const double control_response_max = control_response.empty() ? 0.0 : *std::max_element(control_response.begin(), control_response.end());
  const auto& profile_ledger = simulator.profile_transfer_ledger();
  const auto profile_ledger_bytes = std::accumulate(profile_ledger.cbegin(), profile_ledger.cend(), std::uint64_t{0},
                                                     [](std::uint64_t total, const ProfileTransferLedgerEntry& entry) { return total + entry.bytes; });
  output << std::fixed << std::setprecision(6);
  output << "{\n"
         << "  \"schema_version\": \"1.2\",\n"
         << "  \"scenario_name\": \"" << json_escape(config.scenario_name) << "\",\n"
         << "  \"model_version\": \"" << json_escape(config.model_version) << "\",\n"
         << "  \"workload_mode\": \"" << json_escape(config.workload_mode) << "\",\n"
         << "  \"mapping_plan\": \"" << json_escape(config.mapping_plan.empty() ? "implicit-profile-mapping" : std::filesystem::path(config.mapping_plan).filename().string()) << "\",\n"
         << "  \"scheduler_policy\": \"" << json_escape(config.scheduler_policy) << "\",\n"
         << "  \"arrival_mode\": \"" << json_escape(config.frame_arrival_mode) << "\",\n"
         << "  \"deadline_us\": " << config.deadline_us << ",\n"
         << "  \"assumptions\": {\"npu_arithmetic_energy_scale\": " << config.npu_arithmetic_energy_scale << ", \"arithmetic_voltage_ratio\": " << config.arithmetic_voltage_ratio << ", \"base_static_power_w\": " << config.static_power_w << ", \"npu_static_power_per_mac_w\": " << config.npu_static_power_per_mac_w << ", \"rvv_static_power_per_lane_w\": " << config.rvv_static_power_per_lane_w << ", \"sram_static_power_per_kib_w\": " << config.sram_static_power_per_kib_w << ", \"dram_interface_static_power_w\": " << config.dram_interface_static_power_w << ", \"pue\": " << config.pue << ", \"it_power_budget_w\": " << config.it_power_budget_w << "},\n"
         << "  \"timing\": {\"makespan_us\": " << simulator.makespan_us() << "},\n"
         << "  \"latency\": {\"mean_us\": " << mean << ", \"p99_us\": " << percentile(latency, 0.99) << "},\n"
         << "  \"deadlines\": {\"vision_misses\": " << simulator.deadline_misses() << ", \"vision_jobs\": " << latency.size()
         << ", \"control_misses\": " << simulator.control_deadline_misses() << ", \"control_jobs\": " << simulator.control_tasks() << "},\n"
         << "  \"qos\": {\"vision_queue_delay_mean_us\": " << queue_delay_mean << ", \"vision_queue_delay_max_us\": " << queue_delay_max
         << ", \"output_interval_jitter_max_us\": " << max_output_interval_jitter_us(simulator.vision_completion_times_us(), config.frame_period_us)
         << ", \"control_response_max_us\": " << control_response_max << ", \"max_pending_tasks\": " << simulator.max_pending_tasks() << "},\n"
         << "  \"traffic\": {\"dram_bytes\": " << simulator.total_dram_bytes() << "},\n"
         << "  \"profile_transfer_ledger\": {\"per_frame_bytes\": " << profile_ledger_bytes << ", \"entries\": [";
  for (std::size_t index = 0; index < profile_ledger.size(); ++index) {
    const auto& entry = profile_ledger[index];
    if (index > 0) output << ", ";
    output << "{\"operator_id\": \"" << json_escape(entry.operator_id) << "\", \"category\": \"" << json_escape(entry.category) << "\", \"bytes\": " << entry.bytes << "}";
  }
  output << "]},\n"
         << "  \"mapping_effects\": {\"fusion_count\": " << simulator.profile_fusion_count() << ", \"fused_intermediate_bytes_avoided_per_frame\": " << simulator.profile_fused_intermediate_bytes_avoided() << "},\n"
         << "  \"energy\": {\"per_frame_uj\": " << energy_per_frame_uj << ", \"static_per_frame_uj\": " << static_energy_pj / latency.size() / 1'000'000.0 << ", \"it_per_frame_uj\": " << it_energy_pj / latency.size() / 1'000'000.0 << ", \"facility_per_frame_uj\": " << config.pue * it_energy_pj / latency.size() / 1'000'000.0 << ", \"vector_pj\": " << simulator.vector_energy_pj()
         << ", \"npu_arithmetic_pj\": " << simulator.npu_arithmetic_energy_pj() << ", \"dram_pj\": " << simulator.dram_energy_pj()
         << ", \"dma_pj\": " << simulator.dma_energy_pj() << "},\n"
         << "  \"power\": {\"base_static_w\": " << config.static_power_w << ", \"modeled_static_w\": " << static_power_w << ", \"average_it_w\": " << average_it_power_w << ", \"average_facility_w\": " << config.pue * average_it_power_w << ", \"budget_w\": " << config.it_power_budget_w << ", \"pue\": " << config.pue << ", \"feasible\": " << (config.it_power_budget_w == 0 || average_it_power_w <= config.it_power_budget_w ? "true" : "false") << "},\n"
         << "  \"deployment\": {\"xpu_count\": " << config.deployment_xpu_count << ", \"hours\": " << config.deployment_hours << ", \"electricity_cost_per_kwh\": " << config.electricity_cost_per_kwh << ", \"facility_energy_kwh\": " << deployment_facility_kwh << ", \"electricity_cost\": " << deployment_facility_kwh * config.electricity_cost_per_kwh << "},\n"
         << "  \"area_proxy\": {\"base_mm2\": " << config.base_area_mm2 << ", \"npu_per_mac_mm2\": " << config.npu_area_per_mac_mm2 << ", \"rvv_per_lane_mm2\": " << config.rvv_area_per_lane_mm2 << ", \"sram_per_kib_mm2\": " << config.sram_area_per_kib_mm2 << ", \"modeled_mm2\": " << area_mm2 << ", \"limit_mm2\": " << config.max_area_mm2 << ", \"feasible\": " << (config.max_area_mm2 == 0 || area_mm2 <= config.max_area_mm2 ? "true" : "false") << "},\n"
         << "  \"thermal_proxy\": {\"ambient_c\": " << config.ambient_temp_c << ", \"resistance_c_per_w\": " << config.thermal_resistance_c_per_w << ", \"capacitance_j_per_c\": " << config.thermal_capacitance_j_per_c << ", \"end_c\": " << end_temp_c << ", \"steady_state_c\": " << steady_temp_c << ", \"limit_c\": " << config.max_junction_temp_c << ", \"feasible\": " << (config.max_junction_temp_c == 0 || end_temp_c <= config.max_junction_temp_c ? "true" : "false") << "},\n"
         << "  \"reliability_proxy\": {\"fit_rate\": " << config.reliability_fit_rate << ", \"voltage_multiplier\": " << config.reliability_voltage_fit_multiplier << ", \"expected_failures\": " << expected_failures << "}\n"
         << "}\n";
}

void print_results(const Simulator& simulator, const Config& config) {
  const auto& latency = simulator.latencies_us();
  const double mean = std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size();
  const double energy_per_frame_uj = simulator.total_energy_pj() / latency.size() / 1'000'000.0;
  const double static_power_w = modeled_static_power_w(config);
  const double static_energy_pj = static_power_w * simulator.makespan_us() * 1'000'000.0;
  const double it_energy_pj = simulator.total_energy_pj() + static_energy_pj;
  const double average_it_power_w = simulator.makespan_us() > 0 ? it_energy_pj / simulator.makespan_us() / 1'000'000.0 : 0.0;
  const double deployment_facility_kwh = config.pue * average_it_power_w * config.deployment_xpu_count * config.deployment_hours / 1000.0;
  const double area_mm2 = modeled_area_mm2(config);
  const double steady_temp_c = config.ambient_temp_c + average_it_power_w * config.thermal_resistance_c_per_w;
  const double tau_s = config.thermal_resistance_c_per_w * config.thermal_capacitance_j_per_c;
  const double end_temp_c = tau_s > 0 ? config.ambient_temp_c + (steady_temp_c - config.ambient_temp_c) * (1.0 - std::exp(-(simulator.makespan_us() / 1'000'000.0) / tau_s)) : steady_temp_c;
  const double expected_failures = config.reliability_fit_rate * config.reliability_voltage_fit_multiplier * config.deployment_xpu_count * config.deployment_hours / 1'000'000'000.0;
  const auto& queue_delay = simulator.vision_resource_queue_delays_us();
  const auto& control_response = simulator.control_response_times_us();
  const double queue_delay_mean = queue_delay.empty() ? 0.0 : std::accumulate(queue_delay.begin(), queue_delay.end(), 0.0) / queue_delay.size();
  const double queue_delay_max = queue_delay.empty() ? 0.0 : *std::max_element(queue_delay.begin(), queue_delay.end());
  const double control_response_max = control_response.empty() ? 0.0 : *std::max_element(control_response.begin(), control_response.end());
  const auto per_frame_uj = [&latency](double energy_pj) { return energy_pj / latency.size() / 1'000'000.0; };
  const double makespan = simulator.makespan_us();
  const double vector_ops_per_frame = config.workload_mode == "profiled_graph" ? simulator.profile_vector_ops_per_frame() : static_cast<double>(config.preprocess_elements) * config.preprocess_ops_per_element + (config.workload_mode == "physical_ai" ? static_cast<double>(config.postprocess_elements) * config.postprocess_ops_per_element : 0.0);
  const double npu_macs_per_frame = config.workload_mode == "profiled_graph" ? simulator.profile_npu_macs_per_frame() : config.workload_mode == "physical_ai" ? static_cast<double>(config.gemm_m) * config.gemm_n * config.gemm_k : 0.0;
  const auto utilization = [&simulator, makespan](const std::string& resource) {
    return makespan > 0.0 ? 100.0 * simulator.resource_busy_us(resource) / makespan : 0.0;
  };
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "AxisSim results\n"
            << "  scenario / model:    " << config.scenario_name << " / " << config.model_version << "\n"
            << "  workload mode:        " << config.workload_mode << "\n"
            << "  frame arrivals:       " << config.frame_arrival_mode << "\n"
            << "  RVV timing model:     " << config.rvv_timing_model << "\n"
            << "  RVV memory accounting:" << config.rvv_memory_accounting << "\n"
            << "  arithmetic voltage ratio: " << config.arithmetic_voltage_ratio << " (energy sensitivity only)\n"
            << "  scheduler policy:    " << config.scheduler_policy << "\n"
            << "  frames:              " << latency.size() << "\n"
            << "  RVV VLMAX:           " << simulator.vlmax() << " elements\n"
            << "  workload vector ops / NPU MACs per frame: " << vector_ops_per_frame << " / " << npu_macs_per_frame << "\n"
            << "  RVV tail utilization: " << 100.0 * simulator.tail_utilization(config.preprocess_elements) << "%";
  if (config.workload_mode == "physical_ai") {
    std::cout << " / " << 100.0 * simulator.tail_utilization(config.postprocess_elements) << "%\n"
              << "  NPU tile / SRAM:     " << simulator.npu_tile_working_set_bytes() << " / " << config.sram_capacity_bytes << " bytes\n"
              << "  NPU DRAM first/steady frame: " << static_cast<double>(simulator.npu_transfer_bytes(0)) / 1'000'000.0 << " / "
              << static_cast<double>(simulator.npu_steady_state_dram_bytes_per_frame()) / 1'000'000.0 << " MB\n"
              << "  persistent weights / double buffer: " << (config.npu_weights_resident == 1 ? "on" : "off") << " / "
              << (config.dma_double_buffering == 1 ? "on" : "off") << "\n";
  } else if (config.workload_mode == "rvv_stream") {
    std::cout << "\n  stream scope:         RVV compute only (no DMA, cache, scalar, or NPU stages)\n";
  } else {
    std::cout << "\n  mapping plan:         " << (config.mapping_plan.empty() ? "implicit-profile-mapping" : std::filesystem::path(config.mapping_plan).filename().string())
              << "\n  profile scope:        Static operation/traffic characterization; mapping-policy assumptions, not measured runtime or compiler validation\n";
    if (simulator.profile_fusion_count() > 0) {
      std::cout << "  fused intermediate traffic avoided: " << simulator.profile_fused_intermediate_bytes_avoided() << " bytes / frame\n";
    }
  }
  std::cout << "  latency mean / P99:  " << mean << " / " << percentile(latency, 0.99) << " us\n"
            << "  latency min / max:   " << *std::min_element(latency.begin(), latency.end()) << " / " << *std::max_element(latency.begin(), latency.end()) << " us\n"
            << "  vision queue delay mean / max: " << queue_delay_mean << " / " << queue_delay_max << " us\n"
            << "  output interval jitter max: " << max_output_interval_jitter_us(simulator.vision_completion_times_us(), config.frame_period_us) << " us\n"
            << "  max concurrent tasks:" << simulator.max_concurrent_tasks() << "\n"
            << "  max pending tasks:    " << simulator.max_pending_tasks() << "\n"
            << "  utilization DMA/RVV/NPU: " << utilization("DMA+DRAM") << "% / " << utilization("RVV") << "% / " << utilization("NPU") << "%\n"
            << "  deadline misses:     " << simulator.deadline_misses() << " / " << latency.size() << " (deadline " << config.deadline_us << " us)\n"
            << "  DRAM traffic:        " << static_cast<double>(simulator.total_dram_bytes()) / 1'000'000.0 << " MB\n"
            << "  energy per frame:    " << energy_per_frame_uj << " uJ (modeled estimate)\n"
            << "  energy vector/NPU/DRAM/DMA: " << per_frame_uj(simulator.vector_energy_pj()) << " / "
            << per_frame_uj(simulator.npu_arithmetic_energy_pj()) << " / " << per_frame_uj(simulator.dram_energy_pj())
            << " / " << per_frame_uj(simulator.dma_energy_pj()) << " uJ\n"
            << "  modeled static power: " << static_power_w << " W\n"
            << "  energy static/IT/facility: " << static_energy_pj / latency.size() / 1'000'000.0 << " / " << it_energy_pj / latency.size() / 1'000'000.0 << " / " << config.pue * it_energy_pj / latency.size() / 1'000'000.0 << " uJ\n"
            << "  average IT/facility power: " << average_it_power_w << " / " << config.pue * average_it_power_w << " W\n";
  if (config.it_power_budget_w > 0) std::cout << "  IT power budget:      " << config.it_power_budget_w << " W (" << (average_it_power_w <= config.it_power_budget_w ? "feasible" : "infeasible") << ")\n";
  if (config.deployment_hours > 0) std::cout << "  deployment facility energy / cost: " << deployment_facility_kwh << " kWh / " << deployment_facility_kwh * config.electricity_cost_per_kwh << " (modeled extrapolation)\n";
  if (config.max_area_mm2 > 0 || area_mm2 > 0) std::cout << "  architecture area proxy: " << area_mm2 << " mm^2" << (config.max_area_mm2 > 0 ? (area_mm2 <= config.max_area_mm2 ? " (feasible)" : " (infeasible)") : "") << "\n";
  if (config.thermal_resistance_c_per_w > 0) std::cout << "  first-order thermal proxy end / steady: " << end_temp_c << " / " << steady_temp_c << " C\n";
  if (config.reliability_fit_rate > 0 && config.deployment_hours > 0) std::cout << "  reliability proxy expected failures: " << expected_failures << " (assumption-driven FIT extrapolation)\n";
  if (simulator.control_tasks() > 0) {
    std::cout << "  control deadline misses: " << simulator.control_deadline_misses() << " / " << simulator.control_tasks()
              << " (deadline " << config.control_deadline_us << " us)\n"
              << "  control response max: " << control_response_max << " us\n";
  }
}

[[nodiscard]] std::vector<std::string> split_csv_values(const std::string& values) {
  std::vector<std::string> result;
  std::stringstream stream(values);
  std::string value;
  while (std::getline(stream, value, ',')) {
    value = trim(value);
    if (value.empty()) throw std::runtime_error("Sweep values must be non-empty");
    result.push_back(value);
  }
  if (result.empty()) throw std::runtime_error("Sweep requires at least one value");
  return result;
}

struct SensitivityRange {
  std::string parameter;
  double low = 0.0;
  double nominal = 0.0;
  double high = 0.0;
  std::string unit;
  std::string evidence_class;
  std::string rationale;
};

struct AnalysisPoint {
  std::string analysis;
  std::string parameter;
  std::string setting;
  double mean_latency_us = 0.0;
  double p99_latency_us = 0.0;
  int deadline_misses = 0;
  double it_energy_per_frame_uj = 0.0;
  double average_it_power_w = 0.0;
  double thermal_end_c = 0.0;
  bool feasible = true;
};

[[nodiscard]] const std::set<std::string>& sensitivity_supported_keys() {
  static const std::set<std::string> keys = {
      "dram_bandwidth_gbps", "dma_setup_us", "rvv_frequency_ghz", "npu_frequency_ghz",
      "vector_op_pj", "npu_mac_pj", "npu_arithmetic_energy_scale", "arithmetic_voltage_ratio",
      "dram_byte_pj", "dma_byte_pj", "static_power_w", "npu_static_power_per_mac_w",
      "rvv_static_power_per_lane_w", "sram_static_power_per_kib_w", "dram_interface_static_power_w",
      "it_power_budget_w", "ambient_temp_c", "thermal_resistance_c_per_w", "thermal_capacitance_j_per_c",
      "max_junction_temp_c", "reliability_fit_rate", "reliability_voltage_fit_multiplier"};
  return keys;
}

[[nodiscard]] double strict_csv_double(const std::string& value, const std::string& field) {
  std::size_t consumed = 0;
  double result = 0.0;
  try {
    result = std::stod(value, &consumed);
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid sensitivity " + field + ": " + value);
  }
  if (consumed != value.size() || !std::isfinite(result)) throw std::runtime_error("Invalid sensitivity " + field + ": " + value);
  return result;
}

[[nodiscard]] std::vector<SensitivityRange> read_sensitivity_ranges(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Unable to open sensitivity ranges: " + path);
  std::string header;
  if (!std::getline(input, header) || trim(header) != "parameter,low,nominal,high,unit,evidence_class,rationale") {
    throw std::runtime_error("Sensitivity ranges header must be exactly: parameter,low,nominal,high,unit,evidence_class,rationale");
  }
  std::vector<SensitivityRange> ranges;
  std::set<std::string> seen;
  std::string line;
  while (std::getline(input, line)) {
    if (trim(line).empty() || trim(line).starts_with('#')) continue;
    const auto fields = split_csv_values(line);
    if (fields.size() != 7) throw std::runtime_error("Sensitivity range rows must have seven comma-free fields");
    SensitivityRange range;
    range.parameter = fields[0];
    range.low = strict_csv_double(fields[1], "low bound");
    range.nominal = strict_csv_double(fields[2], "nominal value");
    range.high = strict_csv_double(fields[3], "high bound");
    range.unit = fields[4];
    range.evidence_class = fields[5];
    range.rationale = fields[6];
    if (!sensitivity_supported_keys().contains(range.parameter)) throw std::runtime_error("Unsupported sensitivity parameter: " + range.parameter);
    if (range.low > range.nominal || range.nominal > range.high) throw std::runtime_error("Sensitivity bounds must satisfy low <= nominal <= high for " + range.parameter);
    if (range.unit.empty() || range.rationale.empty()) throw std::runtime_error("Sensitivity unit and rationale must be non-empty");
    if (range.evidence_class != "assumed_sensitivity" && range.evidence_class != "published_reference" && range.evidence_class != "reproduced_reference") {
      throw std::runtime_error("Sensitivity evidence_class must be assumed_sensitivity, published_reference, or reproduced_reference");
    }
    if (!seen.insert(range.parameter).second) throw std::runtime_error("Duplicate sensitivity parameter: " + range.parameter);
    ranges.push_back(std::move(range));
  }
  if (ranges.empty()) throw std::runtime_error("Sensitivity ranges must contain at least one parameter");
  if (ranges.size() > 4) throw std::runtime_error("Sensitivity analysis supports at most four parameters for bounded corner exploration");
  return ranges;
}

[[nodiscard]] std::string format_setting(double value) {
  std::ostringstream stream;
  stream << std::setprecision(12) << value;
  return stream.str();
}

[[nodiscard]] AnalysisPoint evaluate_analysis_point(const std::string& config_path, const std::map<std::string, std::string>& overrides,
                                                     std::string analysis, std::string parameter, std::string setting) {
  const Config config = load_config(config_path, overrides);
  validate(config);
  Simulator simulator(config);
  simulator.run();
  const auto& latency = simulator.latencies_us();
  const double static_power_w = modeled_static_power_w(config);
  const double static_energy_pj = static_power_w * simulator.makespan_us() * 1'000'000.0;
  const double it_energy_pj = simulator.total_energy_pj() + static_energy_pj;
  const double average_it_power_w = simulator.makespan_us() > 0 ? it_energy_pj / simulator.makespan_us() / 1'000'000.0 : 0.0;
  const double steady_temp_c = config.ambient_temp_c + average_it_power_w * config.thermal_resistance_c_per_w;
  const double tau_s = config.thermal_resistance_c_per_w * config.thermal_capacitance_j_per_c;
  const double end_temp_c = tau_s > 0 ? config.ambient_temp_c + (steady_temp_c - config.ambient_temp_c) * (1.0 - std::exp(-(simulator.makespan_us() / 1'000'000.0) / tau_s)) : steady_temp_c;
  const bool power_feasible = config.it_power_budget_w == 0.0 || average_it_power_w <= config.it_power_budget_w;
  const bool area_feasible = config.max_area_mm2 == 0.0 || modeled_area_mm2(config) <= config.max_area_mm2;
  const bool thermal_feasible = config.max_junction_temp_c == 0.0 || end_temp_c <= config.max_junction_temp_c;
  return {std::move(analysis), std::move(parameter), std::move(setting),
          std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size(), percentile(latency, 0.99),
          simulator.deadline_misses(), it_energy_pj / latency.size() / 1'000'000.0,
          average_it_power_w, end_temp_c, power_feasible && area_feasible && thermal_feasible};
}

void write_analysis_csv(const std::vector<AnalysisPoint>& points, std::ostream& output) {
  output << "analysis,parameter,setting,mean_latency_us,p99_latency_us,deadline_misses,it_energy_per_frame_uj,average_it_power_w,thermal_end_c,feasible\n";
  output << std::fixed << std::setprecision(6);
  for (const auto& point : points) {
    output << point.analysis << ',' << point.parameter << ',' << point.setting << ',' << point.mean_latency_us << ',' << point.p99_latency_us << ','
           << point.deadline_misses << ',' << point.it_energy_per_frame_uj << ',' << point.average_it_power_w << ',' << point.thermal_end_c << ','
           << (point.feasible ? "true" : "false") << '\n';
  }
}

void write_analysis_json(const std::vector<SensitivityRange>& ranges, const std::vector<AnalysisPoint>& points, std::ostream& output) {
  const auto [min_latency, max_latency] = std::minmax_element(points.cbegin(), points.cend(), [](const AnalysisPoint& left, const AnalysisPoint& right) {
    return left.mean_latency_us < right.mean_latency_us;
  });
  const auto [min_energy, max_energy] = std::minmax_element(points.cbegin(), points.cend(), [](const AnalysisPoint& left, const AnalysisPoint& right) {
    return left.it_energy_per_frame_uj < right.it_energy_per_frame_uj;
  });
  const bool all_feasible = std::all_of(points.cbegin(), points.cend(), [](const AnalysisPoint& point) { return point.feasible && point.deadline_misses == 0; });
  output << std::fixed << std::setprecision(6);
  output << "{\n  \"schema_version\": \"axissim.sensitivity.v1\",\n"
         << "  \"interpretation\": \"Declared sensitivity bounds, not statistical confidence intervals or measured silicon error bars.\",\n"
         << "  \"decision_stability\": \"" << (all_feasible ? "robust within declared sensitivity bounds" : "conditional: feasibility or deadline outcome changes across declared sensitivity bounds") << "\",\n"
         << "  \"ranges\": [\n";
  for (std::size_t index = 0; index < ranges.size(); ++index) {
    const auto& range = ranges[index];
    output << "    {\"parameter\": \"" << json_escape(range.parameter) << "\", \"low\": " << range.low << ", \"nominal\": " << range.nominal
           << ", \"high\": " << range.high << ", \"unit\": \"" << json_escape(range.unit) << "\", \"evidence_class\": \"" << json_escape(range.evidence_class)
           << "\", \"rationale\": \"" << json_escape(range.rationale) << "\"}" << (index + 1 == ranges.size() ? "\n" : ",\n");
  }
  output << "  ],\n  \"summary\": {\"point_count\": " << points.size() << ", \"latency_min_us\": " << min_latency->mean_latency_us
         << ", \"latency_max_us\": " << max_latency->mean_latency_us << ", \"energy_min_uj\": " << min_energy->it_energy_per_frame_uj
         << ", \"energy_max_uj\": " << max_energy->it_energy_per_frame_uj << "}\n}\n";
}

void run_sensitivity_analysis(const std::string& config_path, const std::map<std::string, std::string>& base_overrides,
                              const std::vector<SensitivityRange>& ranges, std::ostream* csv_output, std::ostream* json_output) {
  std::vector<AnalysisPoint> points;
  for (const auto& range : ranges) {
    for (const auto& [name, value] : std::array<std::pair<const char*, double>, 3>{{{"low", range.low}, {"nominal", range.nominal}, {"high", range.high}}}) {
      auto overrides = base_overrides;
      overrides[range.parameter] = format_setting(value);
      points.push_back(evaluate_analysis_point(config_path, overrides, "one_at_a_time", range.parameter, name));
    }
  }
  std::vector<std::map<std::string, std::string>> corners = {base_overrides};
  for (const auto& range : ranges) {
    std::vector<std::map<std::string, std::string>> next;
    for (const auto& corner : corners) {
      for (const double value : {range.low, range.nominal, range.high}) {
        auto expanded = corner;
        expanded[range.parameter] = format_setting(value);
        next.push_back(std::move(expanded));
      }
    }
    corners = std::move(next);
  }
  for (const auto& corner : corners) {
    std::ostringstream setting;
    bool first = true;
    for (const auto& range : ranges) {
      if (!first) setting << ';';
      setting << range.parameter << '=' << corner.at(range.parameter);
      first = false;
    }
    points.push_back(evaluate_analysis_point(config_path, corner, "corner", "all", setting.str()));
  }
  if (csv_output) write_analysis_csv(points, *csv_output);
  if (json_output) write_analysis_json(ranges, points, *json_output);
}

void write_sweep_csv(const std::string& config_path, const std::map<std::string, std::string>& base_overrides,
                     const std::string& key, const std::vector<std::string>& values, std::ostream& output) {
  output << "scenario_name,model_version,parameter,value,mean_latency_us,p99_latency_us,deadline_misses,dram_traffic_mb,energy_per_frame_uj,vector_energy_uj,npu_arithmetic_energy_uj,dram_energy_uj,dma_energy_uj\n";
  output << std::fixed << std::setprecision(3);
  for (const auto& value : values) {
    auto overrides = base_overrides;
    overrides[key] = value;
    const Config config = load_config(config_path, overrides);
    validate(config);
    Simulator simulator(config);
    simulator.run();
    const auto& latency = simulator.latencies_us();
    const double mean = std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size();
    const double energy = simulator.total_energy_pj() / latency.size() / 1'000'000.0;
    const auto per_frame_uj = [&latency](double energy_pj) { return energy_pj / latency.size() / 1'000'000.0; };
    output << config.scenario_name << ',' << config.model_version << ',' << key << ',' << value << ',' << mean << ',' << percentile(latency, 0.99) << ','
           << simulator.deadline_misses() << ',' << static_cast<double>(simulator.total_dram_bytes()) / 1'000'000.0
           << ',' << energy << ',' << per_frame_uj(simulator.vector_energy_pj()) << ','
           << per_frame_uj(simulator.npu_arithmetic_energy_pj()) << ',' << per_frame_uj(simulator.dram_energy_pj())
           << ',' << per_frame_uj(simulator.dma_energy_pj()) << '\n';
  }
}

void write_calibration_csv(const std::string& config_path, const std::map<std::string, std::string>& base_overrides,
                           const std::string& reference_path, std::ostream& output) {
  std::ifstream input(reference_path);
  if (!input) throw std::runtime_error("Unable to open reference data: " + reference_path);
  const auto reference_directory = std::filesystem::path(reference_path).parent_path();
  std::string header;
  if (!std::getline(input, header)) throw std::runtime_error("Reference data is empty");
  const auto columns = split_csv_values(header);
  const auto find_column = [&columns](const std::string& name) {
    const auto position = std::find(columns.begin(), columns.end(), name);
    if (position == columns.end()) throw std::runtime_error("Reference data missing column: " + name);
    return static_cast<std::size_t>(position - columns.begin());
  };
  const auto id_column = find_column("reference_id");
  const auto status_column = find_column("status");
  const auto source_url_column = find_column("source_url");
  const auto source_revision_column = find_column("source_revision");
  const auto benchmark_column = find_column("benchmark");
  const auto lanes_column = find_column("lanes");
  const auto frequency_column = find_column("frequency_ghz");
  const auto workload_column = find_column("axis_workload");
  const auto config_column = find_column("axis_config");
  const auto unit_column = find_column("unit");
  const auto extraction_column = find_column("extraction_method");
  const auto latency_column = find_column("reference_latency_us");
  const auto cycle_position = std::find(columns.begin(), columns.end(), "reference_cycles");
  const std::optional<std::size_t> cycle_column = cycle_position == columns.end()
      ? std::nullopt
      : std::optional<std::size_t>(static_cast<std::size_t>(cycle_position - columns.begin()));
  output << "reference_id,status,axis_config,lanes,reference_cycles,predicted_cycles,absolute_error_cycles,relative_error_cycles_percent,reference_latency_us,predicted_latency_us,absolute_error_us,relative_error_percent\n";
  output << std::fixed << std::setprecision(3);
  std::string line;
  while (std::getline(input, line)) {
    if (trim(line).empty() || trim(line).starts_with('#')) continue;
    const auto row = split_csv_values(line);
    if (row.size() != columns.size()) throw std::runtime_error("Reference row has wrong column count");
    for (const auto column : {id_column, source_url_column, source_revision_column, benchmark_column, lanes_column,
                              frequency_column, workload_column, config_column, unit_column, extraction_column, latency_column}) {
      if (row[column].empty()) throw std::runtime_error("Reference row has an empty provenance field");
    }
    if (row[status_column] != "published" && row[status_column] != "reproduced") {
      throw std::runtime_error("Reference status must be published or reproduced");
    }
    if (row[unit_column] != "us") throw std::runtime_error("Reference latency unit must be us");
    if (std::stoi(row[lanes_column]) <= 0 || std::stod(row[frequency_column]) <= 0.0 || std::stod(row[latency_column]) <= 0.0) {
      throw std::runtime_error("Reference lanes, frequency, and latency must be positive");
    }
    if (cycle_column && !row[*cycle_column].empty() && std::stod(row[*cycle_column]) <= 0.0) {
      throw std::runtime_error("Reference cycles must be positive when supplied");
    }
    auto overrides = base_overrides;
    overrides["rvv_lanes"] = row[lanes_column];
    const auto axis_config = row[config_column] == "." ? config_path : (reference_directory / row[config_column]).lexically_normal().string();
    const Config config = load_config(axis_config, overrides);
    validate(config);
    const double reference_frequency = std::stod(row[frequency_column]);
    if (std::abs(config.rvv_frequency_ghz - reference_frequency) > 1e-12) {
      throw std::runtime_error("Reference frequency does not match the selected AxisSim configuration");
    }
    Simulator simulator(config);
    simulator.run();
    const auto& latencies = simulator.latencies_us();
    const double predicted = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    const double reference = std::stod(row[latency_column]);
    const double absolute_error = std::abs(predicted - reference);
    const double relative_error = reference == 0.0 ? 0.0 : 100.0 * absolute_error / std::abs(reference);
    output << row[id_column] << ',' << row[status_column] << ',' << row[config_column] << ',' << row[lanes_column] << ',';
    if (cycle_column && !row[*cycle_column].empty()) {
      if (config.workload_mode != "rvv_stream") throw std::runtime_error("Reference cycles require workload_mode=rvv_stream");
      const double reference_cycles = std::stod(row[*cycle_column]);
      const double predicted_cycles = simulator.rvv_stream_compute_cycles();
      const double absolute_cycle_error = std::abs(predicted_cycles - reference_cycles);
      const double relative_cycle_error = 100.0 * absolute_cycle_error / std::abs(reference_cycles);
      output << reference_cycles << ',' << predicted_cycles << ',' << absolute_cycle_error << ',' << relative_cycle_error << ',';
    } else {
      output << ",,,,";
    }
    output << reference << ',' << predicted << ',' << absolute_error << ',' << relative_error << '\n';
  }
}

}  // namespace axissim

int main(int argc, char** argv) {
  using namespace axissim;
  try {
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "profile-validate") {
      const WorkloadProfile profile = load_workload_profile(argv[2]);
      if (argc == 4) (void)load_mapping_plan(argv[3], profile);
      std::cout << "Workload profile is valid: " << argv[2] << " (" << profile.operators.size() << " operators; source " << profile.source_url << ")";
      if (argc == 4) std::cout << "; mapping plan valid: " << argv[3];
      std::cout << '\n';
      return 0;
    }
    if (argc < 3) {
      std::cerr << "Usage: axissim <validate|run|sweep|calibrate|analyze> <config.conf> [options]\n"
                   "       axissim profile-validate <workload-profile.json> [mapping-plan.json]\n";
      return 2;
    }
    const std::string command = argv[1];
    std::optional<std::string> trace_path;
    std::optional<std::string> json_path;
    std::optional<std::string> output_path;
    std::optional<std::string> sweep_assignment;
    std::optional<std::string> reference_path;
    std::optional<std::string> ranges_path;
    std::map<std::string, std::string> overrides;
    for (int argument = 3; argument < argc; ++argument) {
      const std::string option = argv[argument];
      if (option == "--trace") {
        if (++argument >= argc || trace_path.has_value()) throw std::runtime_error("--trace requires one path and may appear once");
        trace_path = argv[argument];
      } else if (option == "--json") {
        if (++argument >= argc || json_path.has_value()) throw std::runtime_error("--json requires one path and may appear once");
        json_path = argv[argument];
      } else if (option == "--csv") {
        if (++argument >= argc || output_path.has_value()) throw std::runtime_error("--csv requires one path and may appear once");
        output_path = argv[argument];
      } else if (option == "--sweep") {
        if (++argument >= argc || sweep_assignment.has_value()) throw std::runtime_error("--sweep requires one key=value1,value2,... assignment and may appear once");
        sweep_assignment = argv[argument];
      } else if (option == "--reference") {
        if (++argument >= argc || reference_path.has_value()) throw std::runtime_error("--reference requires one CSV path and may appear once");
        reference_path = argv[argument];
      } else if (option == "--ranges") {
        if (++argument >= argc || ranges_path.has_value()) throw std::runtime_error("--ranges requires one CSV path and may appear once");
        ranges_path = argv[argument];
      } else if (option == "--set") {
        if (++argument >= argc) throw std::runtime_error("--set requires key=value");
        const std::string assignment = argv[argument];
        const auto separator = assignment.find('=');
        if (separator == std::string::npos || separator == 0 || separator == assignment.size() - 1) throw std::runtime_error("Expected key=value after --set");
        const auto [_, inserted] = overrides.emplace(assignment.substr(0, separator), assignment.substr(separator + 1));
        if (!inserted) throw std::runtime_error("Duplicate override: " + assignment.substr(0, separator));
      } else {
        throw std::runtime_error("Unknown option: " + option);
      }
    }
    if (command == "sweep") {
      if (!sweep_assignment || trace_path || json_path) throw std::runtime_error("sweep requires --sweep and does not support --trace or --json");
      const auto separator = sweep_assignment->find('=');
      if (separator == std::string::npos || separator == 0 || separator == sweep_assignment->size() - 1) throw std::runtime_error("Expected key=value1,value2,... after --sweep");
      const std::string key = sweep_assignment->substr(0, separator);
      if (!supported_keys().contains(key)) throw std::runtime_error("Unsupported sweep key: " + key);
      const auto values = split_csv_values(sweep_assignment->substr(separator + 1));
      if (output_path) {
        std::ofstream output(*output_path);
        if (!output) throw std::runtime_error("Unable to write CSV: " + *output_path);
        write_sweep_csv(argv[2], overrides, key, values, output);
        std::cout << "Wrote " << values.size() << " sweep points to " << *output_path << '\n';
      } else {
        write_sweep_csv(argv[2], overrides, key, values, std::cout);
      }
      return 0;
    }
    if (command == "calibrate") {
      if (!reference_path || trace_path || json_path || sweep_assignment) throw std::runtime_error("calibrate requires --reference and does not support --trace, --json, or --sweep");
      if (output_path) {
        std::ofstream output(*output_path);
        if (!output) throw std::runtime_error("Unable to write CSV: " + *output_path);
        write_calibration_csv(argv[2], overrides, *reference_path, output);
      } else {
        write_calibration_csv(argv[2], overrides, *reference_path, std::cout);
      }
      return 0;
    }
    if (command == "analyze") {
      if (!ranges_path || trace_path || sweep_assignment || reference_path || (!output_path && !json_path)) {
        throw std::runtime_error("analyze requires --ranges and at least one of --csv or --json; it does not support --trace, --sweep, or --reference");
      }
      const auto ranges = read_sensitivity_ranges(*ranges_path);
      std::optional<std::ofstream> csv;
      std::optional<std::ofstream> json;
      if (output_path) {
        csv.emplace(*output_path);
        if (!*csv) throw std::runtime_error("Unable to write CSV: " + *output_path);
      }
      if (json_path) {
        json.emplace(*json_path);
        if (!*json) throw std::runtime_error("Unable to write JSON: " + *json_path);
      }
      run_sensitivity_analysis(argv[2], overrides, ranges, csv ? &*csv : nullptr, json ? &*json : nullptr);
      if (output_path) std::cout << "Wrote sensitivity points to " << *output_path << '\n';
      if (json_path) std::cout << "Wrote sensitivity summary to " << *json_path << '\n';
      return 0;
    }
    if (output_path || sweep_assignment || reference_path || ranges_path) throw std::runtime_error("--csv, --sweep, --reference, and --ranges are only valid with their matching commands");
    const Config config = load_config(argv[2], overrides);
    validate(config);
    if (command == "validate") {
      std::cout << "Config is valid: " << argv[2] << '\n';
      return 0;
    }
    if (command != "run") throw std::runtime_error("Unknown command: " + command);
    Simulator simulator(config);
    simulator.run();
    print_results(simulator, config);
    if (trace_path) {
      write_trace_csv(*trace_path, simulator.trace());
      std::cout << "  trace:               " << *trace_path << '\n';
    }
    if (json_path) {
      write_result_json(*json_path, simulator, config);
      std::cout << "  result JSON:         " << *json_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
