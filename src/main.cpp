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
      "scenario_name", "model_version", "workload_mode", "scheduler_policy", "frames", "frame_period_us", "deadline_us", "dram_bandwidth_gbps", "dma_setup_us", "control_period_us", "control_deadline_us", "control_ops",
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
  if (values.contains("scheduler_policy")) c.scheduler_policy = values.at("scheduler_policy");
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
  if (c.workload_mode != "physical_ai" && c.workload_mode != "rvv_stream") throw std::runtime_error("workload_mode must be physical_ai or rvv_stream");
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
}

struct TraceEvent { int frame; std::string task; std::string resource; double start_us; double end_us; };

class Simulator {
 public:
  explicit Simulator(Config config) : c_(std::move(config)) {}

  void run() {
    run_event_scheduler();
  }

  [[nodiscard]] const std::vector<TraceEvent>& trace() const { return trace_; }
  [[nodiscard]] const std::vector<double>& latencies_us() const { return latencies_us_; }
  [[nodiscard]] int deadline_misses() const { return deadline_misses_; }
  [[nodiscard]] int control_tasks() const { return control_tasks_; }
  [[nodiscard]] int control_deadline_misses() const { return control_deadline_misses_; }
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
  enum class RuntimeStage : int { Camera = 0, PreRead, Preprocess, PreWrite, NpuData, NpuCompute, PostRead, Postprocess, Control, VectorKernel };

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
    }
    return task;
  }

  [[nodiscard]] double transfer_duration_us(std::uint64_t bytes) const {
    return c_.dma_setup_us + static_cast<double>(bytes) / c_.dram_bandwidth_gbps / 1000.0;
  }

  void run_event_scheduler() {
    std::priority_queue<RuntimeTask, std::vector<RuntimeTask>, FutureTaskOrder> future;
    std::priority_queue<Completion, std::vector<Completion>, CompletionOrder> completions;
    std::array<std::vector<RuntimeTask>, 3> pending;
    std::array<bool, 3> busy = {false, false, false};
    const auto initial_stage = c_.workload_mode == "rvv_stream" ? RuntimeStage::VectorKernel : RuntimeStage::Camera;
    if (c_.dma_double_buffering == 1) {
      for (int frame = 0; frame < c_.frames; ++frame) future.push(make_runtime_task(frame, initial_stage, frame * c_.frame_period_us, frame * c_.frame_period_us));
    } else {
      future.push(make_runtime_task(0, initial_stage, 0.0, 0.0));
    }
    if (c_.control_period_us > 0.0) {
      const double horizon_us = c_.frames * c_.frame_period_us;
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
          if (latency > c_.deadline_us) ++deadline_misses_;
          if (c_.dma_double_buffering == 0 && task.frame + 1 < c_.frames) {
            const double next_release = (task.frame + 1) * c_.frame_period_us;
            future.push(make_runtime_task(task.frame + 1, initial_stage, next_release, std::max(next_release, completion.end_us)));
          }
        } else if (task.stage == RuntimeStage::Control) {
          ++control_tasks_;
          if (completion.end_us - task.release_us > c_.control_deadline_us) ++control_deadline_misses_;
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
  std::vector<TraceEvent> trace_;
  std::vector<double> latencies_us_;
  int deadline_misses_ = 0;
  int control_tasks_ = 0;
  int control_deadline_misses_ = 0;
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
  output << std::fixed << std::setprecision(6);
  output << "{\n"
         << "  \"schema_version\": \"1.0\",\n"
         << "  \"scenario_name\": \"" << json_escape(config.scenario_name) << "\",\n"
         << "  \"model_version\": \"" << json_escape(config.model_version) << "\",\n"
         << "  \"workload_mode\": \"" << json_escape(config.workload_mode) << "\",\n"
         << "  \"scheduler_policy\": \"" << json_escape(config.scheduler_policy) << "\",\n"
         << "  \"assumptions\": {\"npu_arithmetic_energy_scale\": " << config.npu_arithmetic_energy_scale << ", \"arithmetic_voltage_ratio\": " << config.arithmetic_voltage_ratio << ", \"base_static_power_w\": " << config.static_power_w << ", \"npu_static_power_per_mac_w\": " << config.npu_static_power_per_mac_w << ", \"rvv_static_power_per_lane_w\": " << config.rvv_static_power_per_lane_w << ", \"sram_static_power_per_kib_w\": " << config.sram_static_power_per_kib_w << ", \"dram_interface_static_power_w\": " << config.dram_interface_static_power_w << ", \"pue\": " << config.pue << ", \"it_power_budget_w\": " << config.it_power_budget_w << "},\n"
         << "  \"timing\": {\"makespan_us\": " << simulator.makespan_us() << "},\n"
         << "  \"latency\": {\"mean_us\": " << mean << ", \"p99_us\": " << percentile(latency, 0.99) << "},\n"
         << "  \"deadlines\": {\"vision_misses\": " << simulator.deadline_misses() << ", \"vision_jobs\": " << latency.size()
         << ", \"control_misses\": " << simulator.control_deadline_misses() << ", \"control_jobs\": " << simulator.control_tasks() << "},\n"
         << "  \"traffic\": {\"dram_bytes\": " << simulator.total_dram_bytes() << "},\n"
         << "  \"energy\": {\"per_frame_uj\": " << energy_per_frame_uj << ", \"static_per_frame_uj\": " << static_energy_pj / latency.size() / 1'000'000.0 << ", \"it_per_frame_uj\": " << it_energy_pj / latency.size() / 1'000'000.0 << ", \"facility_per_frame_uj\": " << config.pue * it_energy_pj / latency.size() / 1'000'000.0 << ", \"vector_pj\": " << simulator.vector_energy_pj()
         << ", \"npu_arithmetic_pj\": " << simulator.npu_arithmetic_energy_pj() << ", \"dram_pj\": " << simulator.dram_energy_pj()
         << ", \"dma_pj\": " << simulator.dma_energy_pj() << "},\n"
         << "  \"power\": {\"base_static_w\": " << config.static_power_w << ", \"modeled_static_w\": " << static_power_w << ", \"average_it_w\": " << average_it_power_w << ", \"average_facility_w\": " << config.pue * average_it_power_w << ", \"budget_w\": " << config.it_power_budget_w << ", \"pue\": " << config.pue << ", \"feasible\": " << (config.it_power_budget_w == 0 || average_it_power_w <= config.it_power_budget_w ? "true" : "false") << "},\n"
         << "  \"deployment\": {\"xpu_count\": " << config.deployment_xpu_count << ", \"hours\": " << config.deployment_hours << ", \"facility_energy_kwh\": " << deployment_facility_kwh << ", \"electricity_cost\": " << deployment_facility_kwh * config.electricity_cost_per_kwh << "}\n"
         << ",  \"area_proxy\": {\"modeled_mm2\": " << area_mm2 << ", \"limit_mm2\": " << config.max_area_mm2 << ", \"feasible\": " << (config.max_area_mm2 == 0 || area_mm2 <= config.max_area_mm2 ? "true" : "false") << "}\n"
         << ",  \"thermal_proxy\": {\"ambient_c\": " << config.ambient_temp_c << ", \"end_c\": " << end_temp_c << ", \"steady_state_c\": " << steady_temp_c << ", \"limit_c\": " << config.max_junction_temp_c << ", \"feasible\": " << (config.max_junction_temp_c == 0 || end_temp_c <= config.max_junction_temp_c ? "true" : "false") << "}\n"
         << ",  \"reliability_proxy\": {\"fit_rate\": " << config.reliability_fit_rate << ", \"voltage_multiplier\": " << config.reliability_voltage_fit_multiplier << ", \"expected_failures\": " << expected_failures << "}\n"
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
  const auto per_frame_uj = [&latency](double energy_pj) { return energy_pj / latency.size() / 1'000'000.0; };
  const double makespan = simulator.makespan_us();
  const double vector_ops_per_frame = static_cast<double>(config.preprocess_elements) * config.preprocess_ops_per_element + (config.workload_mode == "physical_ai" ? static_cast<double>(config.postprocess_elements) * config.postprocess_ops_per_element : 0.0);
  const double npu_macs_per_frame = config.workload_mode == "physical_ai" ? static_cast<double>(config.gemm_m) * config.gemm_n * config.gemm_k : 0.0;
  const auto utilization = [&simulator, makespan](const std::string& resource) {
    return makespan > 0.0 ? 100.0 * simulator.resource_busy_us(resource) / makespan : 0.0;
  };
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "AxisSim results\n"
            << "  scenario / model:    " << config.scenario_name << " / " << config.model_version << "\n"
            << "  workload mode:        " << config.workload_mode << "\n"
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
  } else {
    std::cout << "\n  stream scope:         RVV compute only (no DMA, cache, scalar, or NPU stages)\n";
  }
  std::cout << "  latency mean / P99:  " << mean << " / " << percentile(latency, 0.99) << " us\n"
            << "  latency min / max:   " << *std::min_element(latency.begin(), latency.end()) << " / " << *std::max_element(latency.begin(), latency.end()) << " us\n"
            << "  max concurrent tasks:" << simulator.max_concurrent_tasks() << "\n"
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
              << " (deadline " << config.control_deadline_us << " us)\n";
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
    if (argc < 3) {
      std::cerr << "Usage: axissim <validate|run|sweep|calibrate> <config.conf> [options]\n";
      return 2;
    }
    const std::string command = argv[1];
    std::optional<std::string> trace_path;
    std::optional<std::string> json_path;
    std::optional<std::string> output_path;
    std::optional<std::string> sweep_assignment;
    std::optional<std::string> reference_path;
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
    if (output_path || sweep_assignment || reference_path) throw std::runtime_error("--csv, --sweep, and --reference are only valid with sweep or calibrate");
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
