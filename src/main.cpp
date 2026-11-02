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

namespace axissim {

struct Config {
  std::string scenario_name = "unnamed";
  std::string model_version = "0.1";
  std::string scheduler_policy = "fixed_priority";
  int frames = 30;
  double frame_period_us = 33'333.0;
  double deadline_us = 33'333.0;
  double dram_bandwidth_gbps = 20.0;
  double dma_setup_us = 3.0;
  // A zero period disables the optional high-priority control workload.
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
  double dram_byte_pj = 15.0;
  double dma_byte_pj = 1.5;
};

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
      "scenario_name", "model_version", "scheduler_policy", "frames", "frame_period_us", "deadline_us", "dram_bandwidth_gbps", "dma_setup_us", "control_period_us", "control_deadline_us", "control_ops",
      "rvv_vlen_bits", "rvv_sew_bits", "rvv_lmul", "rvv_lanes", "rvv_ops_per_lane_cycle", "rvv_frequency_ghz", "rvv_stripmine_overhead_cycles",
      "npu_macs", "npu_frequency_ghz", "input_bytes", "preprocess_elements", "preprocess_ops_per_element", "preprocess_output_bytes",
      "gemm_m", "gemm_n", "gemm_k", "npu_tile_m", "npu_tile_n", "npu_tile_k",
      "npu_element_bytes", "npu_accumulator_bytes", "sram_capacity_bytes", "npu_weights_resident", "weight_sram_capacity_bytes", "dma_double_buffering",
      "postprocess_elements", "postprocess_ops_per_element", "postprocess_bytes", "vector_op_pj", "npu_mac_pj",
      "npu_arithmetic_energy_scale", "dram_byte_pj", "dma_byte_pj"};
  return keys;
}

[[nodiscard]] Config load_config(const std::string& path, const std::map<std::string, std::string>& overrides = {}) {
  auto values = read_key_values(path);
  for (const auto& [key, value] : overrides) values[key] = value;
  for (const auto& [key, value] : values) {
    (void)value;
    if (!supported_keys().contains(key)) throw std::runtime_error("Unsupported configuration key: " + key);
  }
  Config c;
  if (values.contains("scenario_name")) c.scenario_name = values.at("scenario_name");
  if (values.contains("model_version")) c.model_version = values.at("model_version");
  if (values.contains("scheduler_policy")) c.scheduler_policy = values.at("scheduler_policy");
#define AXISSIM_LOAD(field) c.field = parse_number(values, #field, c.field)
  AXISSIM_LOAD(frames); AXISSIM_LOAD(frame_period_us); AXISSIM_LOAD(deadline_us);
  AXISSIM_LOAD(dram_bandwidth_gbps); AXISSIM_LOAD(dma_setup_us); AXISSIM_LOAD(control_period_us); AXISSIM_LOAD(control_deadline_us); AXISSIM_LOAD(control_ops);
  AXISSIM_LOAD(rvv_vlen_bits); AXISSIM_LOAD(rvv_sew_bits); AXISSIM_LOAD(rvv_lmul); AXISSIM_LOAD(rvv_lanes);
  AXISSIM_LOAD(rvv_ops_per_lane_cycle); AXISSIM_LOAD(rvv_frequency_ghz); AXISSIM_LOAD(rvv_stripmine_overhead_cycles);
  AXISSIM_LOAD(npu_macs); AXISSIM_LOAD(npu_frequency_ghz);
  AXISSIM_LOAD(input_bytes); AXISSIM_LOAD(preprocess_elements); AXISSIM_LOAD(preprocess_ops_per_element); AXISSIM_LOAD(preprocess_output_bytes);
  AXISSIM_LOAD(gemm_m); AXISSIM_LOAD(gemm_n); AXISSIM_LOAD(gemm_k);
  AXISSIM_LOAD(npu_tile_m); AXISSIM_LOAD(npu_tile_n); AXISSIM_LOAD(npu_tile_k);
  AXISSIM_LOAD(npu_element_bytes); AXISSIM_LOAD(npu_accumulator_bytes); AXISSIM_LOAD(sram_capacity_bytes);
  AXISSIM_LOAD(npu_weights_resident); AXISSIM_LOAD(weight_sram_capacity_bytes); AXISSIM_LOAD(dma_double_buffering);
  AXISSIM_LOAD(postprocess_elements); AXISSIM_LOAD(postprocess_ops_per_element); AXISSIM_LOAD(postprocess_bytes);
  AXISSIM_LOAD(vector_op_pj); AXISSIM_LOAD(npu_mac_pj); AXISSIM_LOAD(npu_arithmetic_energy_scale);
  AXISSIM_LOAD(dram_byte_pj); AXISSIM_LOAD(dma_byte_pj);
#undef AXISSIM_LOAD
  return c;
}

void validate(const Config& c) {
  if (c.frames < 1 || c.frame_period_us <= 0 || c.deadline_us <= 0) throw std::runtime_error("Frame configuration must be positive");
  if (c.scheduler_policy != "fcfs" && c.scheduler_policy != "fixed_priority" && c.scheduler_policy != "edf") throw std::runtime_error("scheduler_policy must be fcfs, fixed_priority, or edf");
  if (c.control_period_us < 0 || c.control_deadline_us < 0 || (c.control_period_us == 0 && (c.control_deadline_us != 0 || c.control_ops != 0)) || (c.control_period_us > 0 && (c.control_deadline_us <= 0 || c.control_ops == 0))) throw std::runtime_error("Control workload must be fully disabled or have positive period, deadline, and operation count");
  if (c.dram_bandwidth_gbps <= 0 || c.rvv_frequency_ghz <= 0 || c.npu_frequency_ghz <= 0) throw std::runtime_error("Bandwidth and frequencies must be positive");
  if (c.rvv_vlen_bits <= 0 || c.rvv_sew_bits <= 0 || c.rvv_vlen_bits % c.rvv_sew_bits != 0 || c.rvv_lmul < 1 || c.rvv_lanes < 1 || c.rvv_ops_per_lane_cycle < 1 || c.rvv_stripmine_overhead_cycles < 0 || c.preprocess_ops_per_element < 1 || c.postprocess_ops_per_element < 1) throw std::runtime_error("Invalid RVV configuration");
  if (c.npu_macs < 1 || c.gemm_m < 1 || c.gemm_n < 1 || c.gemm_k < 1 || c.npu_tile_m < 1 || c.npu_tile_n < 1 || c.npu_tile_k < 1 || c.npu_element_bytes < 1 || c.npu_accumulator_bytes < 1) throw std::runtime_error("NPU dimensions, tiles, element sizes, and MAC count must be positive");
  const auto tile_m = static_cast<std::uint64_t>(std::min(c.gemm_m, c.npu_tile_m));
  const auto tile_n = static_cast<std::uint64_t>(std::min(c.gemm_n, c.npu_tile_n));
  const auto tile_k = static_cast<std::uint64_t>(std::min(c.gemm_k, c.npu_tile_k));
  const auto working_set = (tile_m * tile_k + tile_k * tile_n) * c.npu_element_bytes + tile_m * tile_n * c.npu_accumulator_bytes;
  if (c.sram_capacity_bytes < working_set) throw std::runtime_error("SRAM capacity cannot hold the configured NPU tile working set (requires " + std::to_string(working_set) + " bytes)");
  const auto unique_weight_bytes = static_cast<std::uint64_t>(c.gemm_k) * c.gemm_n * c.npu_element_bytes;
  if ((c.npu_weights_resident != 0 && c.npu_weights_resident != 1) || (c.dma_double_buffering != 0 && c.dma_double_buffering != 1)) throw std::runtime_error("Boolean architecture settings must be 0 or 1");
  if (c.npu_weights_resident == 1 && c.weight_sram_capacity_bytes < unique_weight_bytes) throw std::runtime_error("Weight SRAM cannot hold the configured NPU weight tensor (requires " + std::to_string(unique_weight_bytes) + " bytes)");
  if (c.vector_op_pj < 0 || c.npu_mac_pj < 0 || c.npu_arithmetic_energy_scale <= 0 || c.dram_byte_pj < 0 || c.dma_byte_pj < 0) throw std::runtime_error("Energy values must be non-negative (scale must be positive)");
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
  enum class RuntimeStage : int { Camera = 0, PreRead, Preprocess, PreWrite, NpuData, NpuCompute, PostRead, Postprocess, Control };

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

  [[nodiscard]] RuntimeTask make_runtime_task(int frame, RuntimeStage stage, double release_us, double ready_us) const {
    RuntimeTask task;
    task.frame = frame;
    task.stage = stage;
    task.release_us = release_us;
    task.ready_us = ready_us;
    switch (stage) {
      case RuntimeStage::Camera: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.input_bytes), 0.0, 0.0, c_.input_bytes, 1, "camera_ingest"}; break;
      case RuntimeStage::PreRead: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.input_bytes), 0.0, 0.0, c_.input_bytes, 1, "preprocess_read"}; break;
      case RuntimeStage::Preprocess: task = {frame, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(c_.preprocess_elements, c_.preprocess_ops_per_element), static_cast<double>(c_.preprocess_elements) * c_.preprocess_ops_per_element * c_.vector_op_pj, 0.0, 0, 1, "rvv_preprocess"}; break;
      case RuntimeStage::PreWrite: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.preprocess_output_bytes), 0.0, 0.0, c_.preprocess_output_bytes, 1, "preprocess_write"}; break;
      case RuntimeStage::NpuData: {
        const auto bytes = npu_transfer_bytes(frame);
        task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(bytes), 0.0, 0.0, bytes, 1, "npu_tiled_operands"};
        break;
      }
      case RuntimeStage::NpuCompute: {
        const double macs = static_cast<double>(c_.gemm_m) * c_.gemm_n * c_.gemm_k;
        task = {frame, stage, RuntimeResource::Npu, release_us, ready_us, std::ceil(macs / c_.npu_macs) / (c_.npu_frequency_ghz * 1000.0), 0.0, macs * c_.npu_mac_pj * c_.npu_arithmetic_energy_scale, 0, 1, "npu_gemm"};
        break;
      }
      case RuntimeStage::PostRead: task = {frame, stage, RuntimeResource::Dma, release_us, ready_us, transfer_duration_us(c_.postprocess_bytes), 0.0, 0.0, c_.postprocess_bytes, 1, "postprocess_read"}; break;
      case RuntimeStage::Postprocess: task = {frame, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(c_.postprocess_elements, c_.postprocess_ops_per_element), static_cast<double>(c_.postprocess_elements) * c_.postprocess_ops_per_element * c_.vector_op_pj, 0.0, 0, 1, "rvv_postprocess"}; break;
      case RuntimeStage::Control: task = {-1, stage, RuntimeResource::Rvv, release_us, ready_us, vector_duration_us(c_.control_ops, 1), static_cast<double>(c_.control_ops) * c_.vector_op_pj, 0.0, 0, 0, "safety_control"}; break;
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
    if (c_.dma_double_buffering == 1) {
      for (int frame = 0; frame < c_.frames; ++frame) future.push(make_runtime_task(frame, RuntimeStage::Camera, frame * c_.frame_period_us, frame * c_.frame_period_us));
    } else {
      future.push(make_runtime_task(0, RuntimeStage::Camera, 0.0, 0.0));
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
        if (task.stage == RuntimeStage::Postprocess) {
          const double latency = completion.end_us - task.release_us;
          latencies_us_.push_back(latency);
          if (latency > c_.deadline_us) ++deadline_misses_;
          if (c_.dma_double_buffering == 0 && task.frame + 1 < c_.frames) {
            const double next_release = (task.frame + 1) * c_.frame_period_us;
            future.push(make_runtime_task(task.frame + 1, RuntimeStage::Camera, next_release, std::max(next_release, completion.end_us)));
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
    const double lane_rate = static_cast<double>(c_.rvv_lanes) * c_.rvv_ops_per_lane_cycle;
    const double compute_cycles = std::ceil(static_cast<double>(elements) * ops_per_element / lane_rate);
    const auto iterations = (elements + static_cast<std::uint64_t>(vlmax()) - 1) / static_cast<std::uint64_t>(vlmax());
    const double cycles = compute_cycles + iterations * c_.rvv_stripmine_overhead_cycles;
    return cycles / (c_.rvv_frequency_ghz * 1000.0);
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

void print_results(const Simulator& simulator, const Config& config) {
  const auto& latency = simulator.latencies_us();
  const double mean = std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size();
  const double energy_per_frame_uj = simulator.total_energy_pj() / latency.size() / 1'000'000.0;
  const auto per_frame_uj = [&latency](double energy_pj) { return energy_pj / latency.size() / 1'000'000.0; };
  const double makespan = simulator.makespan_us();
  const auto utilization = [&simulator, makespan](const std::string& resource) {
    return makespan > 0.0 ? 100.0 * simulator.resource_busy_us(resource) / makespan : 0.0;
  };
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "AxisSim results\n"
            << "  scenario / model:    " << config.scenario_name << " / " << config.model_version << "\n"
            << "  scheduler policy:    " << config.scheduler_policy << "\n"
            << "  frames:              " << latency.size() << "\n"
            << "  RVV VLMAX:           " << simulator.vlmax() << " elements\n"
            << "  RVV tail utilization (pre/post): " << 100.0 * simulator.tail_utilization(config.preprocess_elements) << "% / "
            << 100.0 * simulator.tail_utilization(config.postprocess_elements) << "%\n"
            << "  NPU tile / SRAM:     " << simulator.npu_tile_working_set_bytes() << " / " << config.sram_capacity_bytes << " bytes\n"
            << "  NPU DRAM first/steady frame: " << static_cast<double>(simulator.npu_transfer_bytes(0)) / 1'000'000.0 << " / "
            << static_cast<double>(simulator.npu_steady_state_dram_bytes_per_frame()) / 1'000'000.0 << " MB\n"
            << "  persistent weights / double buffer: " << (config.npu_weights_resident == 1 ? "on" : "off") << " / "
            << (config.dma_double_buffering == 1 ? "on" : "off") << "\n"
            << "  latency mean / P99:  " << mean << " / " << percentile(latency, 0.99) << " us\n"
            << "  latency min / max:   " << *std::min_element(latency.begin(), latency.end()) << " / " << *std::max_element(latency.begin(), latency.end()) << " us\n"
            << "  max concurrent tasks:" << simulator.max_concurrent_tasks() << "\n"
            << "  utilization DMA/RVV/NPU: " << utilization("DMA+DRAM") << "% / " << utilization("RVV") << "% / " << utilization("NPU") << "%\n"
            << "  deadline misses:     " << simulator.deadline_misses() << " / " << latency.size() << " (deadline " << config.deadline_us << " us)\n"
            << "  DRAM traffic:        " << static_cast<double>(simulator.total_dram_bytes()) / 1'000'000.0 << " MB\n"
            << "  energy per frame:    " << energy_per_frame_uj << " uJ (modeled estimate)\n"
            << "  energy vector/NPU/DRAM/DMA: " << per_frame_uj(simulator.vector_energy_pj()) << " / "
            << per_frame_uj(simulator.npu_arithmetic_energy_pj()) << " / " << per_frame_uj(simulator.dram_energy_pj())
            << " / " << per_frame_uj(simulator.dma_energy_pj()) << " uJ\n";
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

}  // namespace axissim

int main(int argc, char** argv) {
  using namespace axissim;
  try {
    if (argc < 3) {
      std::cerr << "Usage: axissim <validate|run|sweep> <config.conf> [options]\n";
      return 2;
    }
    const std::string command = argv[1];
    std::optional<std::string> trace_path;
    std::optional<std::string> output_path;
    std::optional<std::string> sweep_assignment;
    std::map<std::string, std::string> overrides;
    for (int argument = 3; argument < argc; ++argument) {
      const std::string option = argv[argument];
      if (option == "--trace") {
        if (++argument >= argc || trace_path.has_value()) throw std::runtime_error("--trace requires one path and may appear once");
        trace_path = argv[argument];
      } else if (option == "--csv") {
        if (++argument >= argc || output_path.has_value()) throw std::runtime_error("--csv requires one path and may appear once");
        output_path = argv[argument];
      } else if (option == "--sweep") {
        if (++argument >= argc || sweep_assignment.has_value()) throw std::runtime_error("--sweep requires one key=value1,value2,... assignment and may appear once");
        sweep_assignment = argv[argument];
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
      if (!sweep_assignment || trace_path) throw std::runtime_error("sweep requires --sweep and does not support --trace");
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
    if (output_path || sweep_assignment) throw std::runtime_error("--csv and --sweep are only valid with the sweep command");
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
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
