#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "profile.hpp"
#include "scheduler_ordering.hpp"
#include "study_measurements.hpp"

namespace axissim {

struct TraceEvent { int frame; std::string task; std::string resource; double start_us; double end_us; };

class Simulator {
 public:
  explicit Simulator(Config config) : c_(std::move(config)) {}

  void run() {
    frame_arrivals_us_ = load_frame_arrivals(c_);
    if (c_.workload_mode == "profiled_graph") {
      profile_ = load_workload_profile(c_.workload_profile);
      if (!c_.mapping_plan.empty()) apply_mapping_plan(profile_, load_mapping_plan(c_.mapping_plan, profile_), c_);
      run_profile_scheduler(nullptr);
    } else {
      run_event_scheduler(nullptr);
    }
  }

  [[nodiscard]] StudyMeasurementResult run_study(const StudyMeasurementRequest& request,
                                                 const WorkloadProfile* resolved_profile = nullptr,
                                                 const std::vector<double>* resolved_frame_arrivals = nullptr) {
    frame_arrivals_us_ = resolved_frame_arrivals != nullptr ? *resolved_frame_arrivals : load_frame_arrivals(c_);
    if (c_.workload_mode == "profiled_graph") {
      if (resolved_profile != nullptr) profile_ = *resolved_profile;
      else {
        profile_ = load_workload_profile(c_.workload_profile);
        if (!c_.mapping_plan.empty()) apply_mapping_plan(profile_, load_mapping_plan(c_.mapping_plan, profile_), c_);
      }
    }

    std::vector<StudyWorkUnitSpec> schedule;
    schedule.reserve(frame_arrivals_us_.size());
    for (std::size_t frame = 0; frame < frame_arrivals_us_.size(); ++frame) {
      const double release = c_.workload_mode != "profiled_graph" && c_.dma_double_buffering == 0 && frame == 0
          ? 0.0 : frame_arrivals_us_[frame];
      schedule.push_back({"frame:" + std::to_string(frame), StudyWorkUnitKind::Frame, frame,
                          release, c_.deadline_us});
    }
    if (c_.control_period_us > 0.0) {
      const double horizon_us = frame_arrivals_us_.back() + c_.frame_period_us;
      std::uint64_t sequence = 0;
      for (double release = 0.0; release < horizon_us; release += c_.control_period_us, ++sequence) {
        schedule.push_back({"control:" + std::to_string(sequence), StudyWorkUnitKind::Control, sequence,
                            release, c_.control_deadline_us});
      }
    }

    std::optional<StudyMeasurementObserver> observer;
    try {
      observer.emplace(request, std::move(schedule));
    } catch (const std::exception& error) {
      return StudyMeasurementObserver::invalid_result(request.mode, std::string("invalid_measurement_request: ") + error.what());
    }
    if (!observer->is_running()) return observer->finish(c_, total_energy_pj_);
    if (c_.workload_mode == "profiled_graph") run_profile_scheduler(&*observer);
    else run_event_scheduler(&*observer);
    observer->fail_if_unterminated("study_scheduler_terminated_before_measurement_completion");
    return observer->finish(c_, total_energy_pj_);
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
  [[nodiscard]] std::uint64_t profile_persistent_weight_bytes_avoided_per_steady_frame() const { return profile_.persistent_weight_bytes_avoided_per_steady_frame; }
  [[nodiscard]] int profile_persistent_weight_operator_count() const { return profile_.persistent_weight_operator_count; }
  [[nodiscard]] std::uint64_t profile_precision_traffic_bytes_avoided_per_frame() const { return profile_.precision_traffic_bytes_avoided_per_frame; }
  [[nodiscard]] const std::string& profile_source_storage_precision() const { return profile_.source_storage_precision; }
  [[nodiscard]] const std::string& profile_target_storage_precision() const { return profile_.target_storage_precision; }
  [[nodiscard]] double profile_mapped_npu_arithmetic_energy_scale() const { return profile_.mapped_npu_arithmetic_energy_scale; }
  [[nodiscard]] const WorkloadProfile& profile() const { return profile_; }
  [[nodiscard]] std::uint64_t profile_first_frame_transfer_bytes() const { return profile_first_frame_transfer_bytes_; }
  [[nodiscard]] std::uint64_t profile_steady_state_transfer_bytes() const { return profile_steady_state_transfer_bytes_; }
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
    std::uint64_t control_sequence = 0;
    std::uint64_t enqueue_sequence = 0;
  };

  struct FutureTaskOrder {
    bool deterministic_v1 = false;
    bool operator()(const RuntimeTask& left, const RuntimeTask& right) const {
      return detail::scheduler_event_later({left.ready_us, left.enqueue_sequence},
                                           {right.ready_us, right.enqueue_sequence}, deterministic_v1);
    }
  };
  struct Completion { double end_us; RuntimeResource resource; RuntimeTask task; std::uint64_t dispatch_sequence = 0; };
  struct CompletionOrder {
    bool deterministic_v1 = false;
    bool operator()(const Completion& left, const Completion& right) const {
      return detail::scheduler_event_later({left.end_us, left.dispatch_sequence},
                                           {right.end_us, right.dispatch_sequence}, deterministic_v1);
    }
  };

  void enqueue_future(std::priority_queue<RuntimeTask, std::vector<RuntimeTask>, FutureTaskOrder>& future,
                      RuntimeTask task, std::uint64_t& next_sequence) const {
    if (c_.scheduler_ordering_policy == "deterministic_v1") task.enqueue_sequence = detail::take_scheduler_sequence(next_sequence);
    future.push(std::move(task));
  }

  void enqueue_completion(std::priority_queue<Completion, std::vector<Completion>, CompletionOrder>& completions,
                          double end_us, RuntimeResource resource, RuntimeTask task,
                          std::uint64_t& next_sequence) const {
    const auto sequence = c_.scheduler_ordering_policy == "deterministic_v1" ? detail::take_scheduler_sequence(next_sequence) : 0;
    completions.push({end_us, resource, std::move(task), sequence});
  }

  [[nodiscard]] bool resource_task_precedes(const RuntimeTask& left, const RuntimeTask& right) const {
    if (c_.scheduler_ordering_policy == "legacy_implicit") {
      if (c_.scheduler_policy == "fixed_priority" && left.priority != right.priority) return left.priority < right.priority;
      if (c_.scheduler_policy == "edf") {
        const double left_deadline = left.release_us + (left.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
        const double right_deadline = right.release_us + (right.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
        if (left_deadline != right_deadline) return left_deadline < right_deadline;
      }
      return left.ready_us < right.ready_us;
    }

    if (c_.scheduler_policy == "fixed_priority" && left.priority != right.priority) return left.priority < right.priority;
    if (c_.scheduler_policy == "edf") {
      const double left_deadline = left.release_us + (left.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
      const double right_deadline = right.release_us + (right.stage == RuntimeStage::Control ? c_.control_deadline_us : c_.deadline_us);
      if (detail::scheduler_time_precedes(left_deadline, right_deadline)) return true;
      if (detail::scheduler_time_precedes(right_deadline, left_deadline)) return false;
    }
    if (detail::scheduler_time_precedes(left.ready_us, right.ready_us)) return true;
    if (detail::scheduler_time_precedes(right.ready_us, left.ready_us)) return false;
    return left.enqueue_sequence < right.enqueue_sequence;
  }

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

  [[nodiscard]] std::string study_work_unit_id(const RuntimeTask& task) const {
    return task.stage == RuntimeStage::Control
        ? "control:" + std::to_string(task.control_sequence)
        : "frame:" + std::to_string(task.frame);
  }

  [[nodiscard]] StudyTaskEnergyPj runtime_task_energy(const RuntimeTask& task) const {
    StudyTaskEnergyPj energy;
    energy.vector = task.vector_energy_pj;
    energy.npu_arithmetic = task.npu_energy_pj;
    energy.dram = static_cast<double>(task.transfer_bytes) * c_.dram_byte_pj;
    energy.dma = static_cast<double>(task.transfer_bytes) * c_.dma_byte_pj;
    // Keep this expression in the same evaluation order as legacy aggregate accounting.
    energy.total = task.vector_energy_pj + task.npu_energy_pj
        + static_cast<double>(task.transfer_bytes) * (c_.dram_byte_pj + c_.dma_byte_pj);
    return energy;
  }

  [[nodiscard]] bool observe_study_dispatch(StudyMeasurementObserver* observer, const RuntimeTask& task,
                                             double start_us, double end_us, const std::string& resource,
                                             const StudyTaskEnergyPj& energy) {
    return observer == nullptr || observer->record_task_dispatch(start_us, study_work_unit_id(task), task.name,
                                                                   resource, task.release_us, task.ready_us, start_us, end_us, energy);
  }

  void accumulate_runtime_energy(const RuntimeTask& task, const StudyTaskEnergyPj& energy) {
    vector_energy_pj_ += energy.vector;
    npu_arithmetic_energy_pj_ += energy.npu_arithmetic;
    if (task.transfer_bytes > 0) {
      total_dram_bytes_ += task.transfer_bytes;
      dram_energy_pj_ += energy.dram;
      dma_energy_pj_ += energy.dma;
    }
    total_energy_pj_ += energy.total;
  }

  [[nodiscard]] StudyQueueSnapshot study_queue_snapshot(double time_us,
                                                          const std::array<std::vector<RuntimeTask>, 3>& pending,
                                                          const std::priority_queue<Completion, std::vector<Completion>, CompletionOrder>& completions,
                                                          std::optional<std::uint64_t> dependency_blocked = std::nullopt) const {
    StudyQueueSnapshot snapshot;
    snapshot.timestamp_us = time_us;
    snapshot.dependency_blocked_tasks = dependency_blocked;
    for (std::size_t resource = 0; resource < pending.size(); ++resource) {
      snapshot.ready_by_resource[resource] = static_cast<std::uint64_t>(pending[resource].size());
    }
    auto completion_copy = completions;
    while (!completion_copy.empty()) {
      const auto resource = static_cast<std::size_t>(completion_copy.top().resource);
      ++snapshot.in_flight_by_resource[resource];
      completion_copy.pop();
    }
    return snapshot;
  }

  template <typename FutureQueue>
  [[nodiscard]] static bool has_same_time_events(double time_us, const FutureQueue& future,
                                                  const std::priority_queue<Completion, std::vector<Completion>, CompletionOrder>& completions) {
    return (!future.empty() && future.top().ready_us <= time_us)
        || (!completions.empty() && completions.top().end_us <= time_us);
  }

  void run_profile_scheduler(StudyMeasurementObserver* observer) {
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
    profile_first_frame_transfer_bytes_ = profile_bytes;
    if (profile_.persistent_weight_bytes_avoided_per_steady_frame > profile_first_frame_transfer_bytes_) {
      throw std::runtime_error("Persistent weight accounting exceeds the profiled first-frame transfer total");
    }
    profile_steady_state_transfer_bytes_ = profile_first_frame_transfer_bytes_ - profile_.persistent_weight_bytes_avoided_per_steady_frame;
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
    const bool deterministic_v1 = c_.scheduler_ordering_policy == "deterministic_v1";
    std::priority_queue<RuntimeTask, std::vector<RuntimeTask>, FutureTaskOrder> future(FutureTaskOrder{deterministic_v1});
    std::priority_queue<Completion, std::vector<Completion>, CompletionOrder> completions(CompletionOrder{deterministic_v1});
    std::uint64_t next_enqueue_sequence = 0;
    std::uint64_t next_completion_sequence = 0;
    std::array<std::vector<RuntimeTask>, 3> pending;
    std::array<bool, 3> busy = {false, false, false};
    std::vector<std::vector<int>> remaining(static_cast<std::size_t>(c_.frames), base_dependencies);
    std::vector<int> remaining_terminals(static_cast<std::size_t>(c_.frames), terminals_per_frame);
    std::vector<double> terminal_end(static_cast<std::size_t>(c_.frames), 0.0);

    const auto transfer_bytes_for_frame = [](const ProfileOperator& operation, int frame) {
      return operation.transfer_bytes - (operation.persistent_weights && frame > 0 ? operation.weight_bytes : 0);
    };
    const auto make_profile_task = [this, &operations, &transfer_bytes_for_frame](int frame, int profile_index, bool transfer, double release, double ready) {
      const auto& operation = operations[static_cast<std::size_t>(profile_index)];
      RuntimeTask task;
      task.frame = frame;
      task.stage = transfer ? RuntimeStage::ProfileTransfer : RuntimeStage::ProfileCompute;
      task.release_us = release;
      task.ready_us = ready;
      task.profile_index = profile_index;
      if (transfer) {
        const auto bytes = transfer_bytes_for_frame(operation, frame);
        task.resource = RuntimeResource::Dma;
        task.duration_us = transfer_duration_us(bytes);
        task.transfer_bytes = bytes;
        task.name = "profile_dma:" + operation.id;
      } else if (operation.mapping_class == "npu_compute") {
        task.resource = RuntimeResource::Npu;
        task.duration_us = std::ceil(static_cast<double>(operation.macs) / c_.npu_macs) / (c_.npu_frequency_ghz * 1000.0);
        task.npu_energy_pj = static_cast<double>(operation.macs) * c_.npu_mac_pj * c_.npu_arithmetic_energy_scale * operation.npu_arithmetic_energy_scale * c_.arithmetic_voltage_ratio * c_.arithmetic_voltage_ratio;
        task.name = "profile_npu:" + operation.id;
      } else {
        task.resource = RuntimeResource::Rvv;
        task.duration_us = vector_duration_us(operation.vector_elements, operation.ops_per_element);
        task.vector_energy_pj = static_cast<double>(operation.vector_elements) * operation.ops_per_element * c_.vector_op_pj;
        task.name = "profile_rvv:" + operation.id;
      }
      return task;
    };
    const auto enqueue_operation = [this, &future, &make_profile_task, &operations, &transfer_bytes_for_frame,
                                    &next_enqueue_sequence](int frame, int operation, double release, double ready) {
      const auto& profile_operation = operations[static_cast<std::size_t>(operation)];
      enqueue_future(future, make_profile_task(frame, operation, transfer_bytes_for_frame(profile_operation, frame) > 0, release, ready), next_enqueue_sequence);
    };
    for (int frame = 0; frame < c_.frames; ++frame) {
      const double release = frame_arrivals_us_[static_cast<std::size_t>(frame)];
      if (observer != nullptr && !observer->includes_work_unit("frame:" + std::to_string(frame))) continue;
      for (const int root : roots) enqueue_operation(frame, root, release, release);
    }
    if (c_.control_period_us > 0.0) {
      const double horizon_us = frame_arrivals_us_.back() + c_.frame_period_us;
      std::uint64_t sequence = 0;
      for (double release = 0.0; release < horizon_us; release += c_.control_period_us, ++sequence) {
        const std::string id = "control:" + std::to_string(sequence);
        if (observer != nullptr && !observer->includes_work_unit(id)) continue;
        RuntimeTask task = make_runtime_task(-1, RuntimeStage::Control, release, release);
        task.control_sequence = sequence;
        enqueue_future(future, std::move(task), next_enqueue_sequence);
      }
    }
    const auto resource_name = [](RuntimeResource resource) { return resource == RuntimeResource::Dma ? "DMA+DRAM" : resource == RuntimeResource::Rvv ? "RVV" : "NPU"; };
    double time = 0.0;
    while (observer == nullptr ? (!future.empty() || !completions.empty()) : observer->is_running()) {
      const double next_future = future.empty() ? std::numeric_limits<double>::infinity() : future.top().ready_us;
      const double next_completion = completions.empty() ? std::numeric_limits<double>::infinity() : completions.top().end_us;
      time = std::min(next_future, next_completion);
      if (observer != nullptr) time = std::min(time, observer->next_observer_time_us());
      if (!std::isfinite(time)) break;
      if (observer != nullptr && !observer->process_releases_through(time)) break;
      while (!future.empty() && future.top().ready_us <= time) {
        if (observer != nullptr && !observer->record_ready_event(time)) break;
        RuntimeTask task = future.top(); future.pop(); pending[static_cast<std::size_t>(task.resource)].push_back(std::move(task));
      }
      if (observer != nullptr && !observer->is_running()) break;
      int pending_count = 0;
      for (const auto& queue : pending) pending_count += static_cast<int>(queue.size());
      max_pending_tasks_ = std::max(max_pending_tasks_, pending_count);
      while (!completions.empty() && completions.top().end_us <= time) {
        if (observer != nullptr && !observer->record_task_completion_event(time)) break;
        Completion completion = completions.top(); completions.pop();
        busy[static_cast<std::size_t>(completion.resource)] = false;
        const RuntimeTask& task = completion.task;
        if (observer != nullptr) observer->record_task_completed(study_work_unit_id(task), task.name, completion.end_us);
        if (task.stage == RuntimeStage::ProfileTransfer) {
          enqueue_future(future, make_profile_task(task.frame, task.profile_index, false, task.release_us, completion.end_us), next_enqueue_sequence);
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
              if (observer != nullptr) observer->record_work_unit_completion("frame:" + std::to_string(task.frame), end);
            }
          }
        } else if (task.stage == RuntimeStage::Control) {
          ++control_tasks_;
          const double response = completion.end_us - task.release_us;
          control_response_times_us_.push_back(response);
          if (response > c_.control_deadline_us) ++control_deadline_misses_;
          if (observer != nullptr) observer->record_work_unit_completion("control:" + std::to_string(task.control_sequence), completion.end_us);
        }
      }
      if (observer != nullptr && !observer->is_running()) break;
      for (std::size_t index = 0; index < pending.size(); ++index) {
        if (busy[index] || pending[index].empty()) continue;
        auto& queue = pending[index];
        const auto selected = std::min_element(queue.begin(), queue.end(), [this](const RuntimeTask& left, const RuntimeTask& right) {
          return resource_task_precedes(left, right);
        });
        RuntimeTask task = *selected;
        const double end = time + task.duration_us;
        const auto task_energy = runtime_task_energy(task);
        if (!observe_study_dispatch(observer, task, time, end, resource_name(task.resource), task_energy)) break;
        queue.erase(selected); busy[index] = true;
        if (task.frame >= 0) vision_resource_queue_delays_us_.push_back(time - task.ready_us);
        trace_.push_back({task.frame, task.name, resource_name(task.resource), time, end});
        accumulate_runtime_energy(task, task_energy);
        const auto resource = task.resource;
        enqueue_completion(completions, end, resource, std::move(task), next_completion_sequence);
        if (observer != nullptr && !observer->is_running()) break;
      }
      if (observer != nullptr && !observer->is_running()) break;
      if (observer != nullptr && !has_same_time_events(time, future, completions)) {
        std::uint64_t dependency_blocked = 0;
        for (int frame = 0; frame < c_.frames; ++frame) {
          if (frame_arrivals_us_[static_cast<std::size_t>(frame)] > time
              || !observer->includes_work_unit("frame:" + std::to_string(frame))) continue;
          for (const auto remaining_dependencies : remaining[static_cast<std::size_t>(frame)]) {
            if (remaining_dependencies > 0) ++dependency_blocked;
          }
        }
        const auto snapshot = study_queue_snapshot(time, pending, completions, dependency_blocked);
        if (!observer->finish_quiescent_timestamp(time, snapshot)) break;
      }
    }
  }

  void run_event_scheduler(StudyMeasurementObserver* observer) {
    const bool deterministic_v1 = c_.scheduler_ordering_policy == "deterministic_v1";
    std::priority_queue<RuntimeTask, std::vector<RuntimeTask>, FutureTaskOrder> future(FutureTaskOrder{deterministic_v1});
    std::priority_queue<Completion, std::vector<Completion>, CompletionOrder> completions(CompletionOrder{deterministic_v1});
    std::uint64_t next_enqueue_sequence = 0;
    std::uint64_t next_completion_sequence = 0;
    std::array<std::vector<RuntimeTask>, 3> pending;
    std::array<bool, 3> busy = {false, false, false};
    const auto initial_stage = c_.workload_mode == "rvv_stream" ? RuntimeStage::VectorKernel : RuntimeStage::Camera;
    if (c_.dma_double_buffering == 1) {
      for (int frame = 0; frame < c_.frames; ++frame) {
        if (observer != nullptr && !observer->includes_work_unit("frame:" + std::to_string(frame))) continue;
        enqueue_future(future, make_runtime_task(frame, initial_stage, frame_arrivals_us_[static_cast<std::size_t>(frame)], frame_arrivals_us_[static_cast<std::size_t>(frame)]), next_enqueue_sequence);
      }
    } else {
      enqueue_future(future, make_runtime_task(0, initial_stage, 0.0, 0.0), next_enqueue_sequence);
    }
    if (c_.control_period_us > 0.0) {
      const double horizon_us = frame_arrivals_us_.back() + c_.frame_period_us;
      std::uint64_t sequence = 0;
      for (double release = 0.0; release < horizon_us; release += c_.control_period_us, ++sequence) {
        const std::string id = "control:" + std::to_string(sequence);
        if (observer != nullptr && !observer->includes_work_unit(id)) continue;
        RuntimeTask task = make_runtime_task(-1, RuntimeStage::Control, release, release);
        task.control_sequence = sequence;
        enqueue_future(future, std::move(task), next_enqueue_sequence);
      }
    }
    auto resource_name = [](RuntimeResource resource) { return resource == RuntimeResource::Dma ? "DMA+DRAM" : resource == RuntimeResource::Rvv ? "RVV" : "NPU"; };
    double time = 0.0;
    while (observer == nullptr ? (!future.empty() || !completions.empty()) : observer->is_running()) {
      const double next_future = future.empty() ? std::numeric_limits<double>::infinity() : future.top().ready_us;
      const double next_completion = completions.empty() ? std::numeric_limits<double>::infinity() : completions.top().end_us;
      time = std::min(next_future, next_completion);
      if (observer != nullptr) time = std::min(time, observer->next_observer_time_us());
      if (!std::isfinite(time)) break;
      if (observer != nullptr && !observer->process_releases_through(time)) break;
      while (!future.empty() && future.top().ready_us <= time) {
        if (observer != nullptr && !observer->record_ready_event(time)) break;
        RuntimeTask task = future.top(); future.pop();
        pending[static_cast<std::size_t>(task.resource)].push_back(std::move(task));
      }
      if (observer != nullptr && !observer->is_running()) break;
      int pending_count = 0;
      for (const auto& queue : pending) pending_count += static_cast<int>(queue.size());
      max_pending_tasks_ = std::max(max_pending_tasks_, pending_count);
      while (!completions.empty() && completions.top().end_us <= time) {
        if (observer != nullptr && !observer->record_task_completion_event(time)) break;
        Completion completion = completions.top(); completions.pop();
        busy[static_cast<std::size_t>(completion.resource)] = false;
        const RuntimeTask& task = completion.task;
        if (observer != nullptr) observer->record_task_completed(study_work_unit_id(task), task.name, completion.end_us);
        const auto total_vector_elements = (task.stage == RuntimeStage::Preprocess || task.stage == RuntimeStage::VectorKernel) ? c_.preprocess_elements : task.stage == RuntimeStage::Postprocess ? c_.postprocess_elements : 0;
        if (total_vector_elements > 0 && task.vector_offset + task.vector_elements < total_vector_elements) {
          enqueue_future(future, make_runtime_task(task.frame, task.stage, task.release_us, completion.end_us, task.vector_offset + task.vector_elements), next_enqueue_sequence);
        } else if (task.stage == RuntimeStage::Postprocess || task.stage == RuntimeStage::VectorKernel) {
          const double latency = completion.end_us - task.release_us;
          latencies_us_.push_back(latency);
          vision_completion_times_us_.push_back(completion.end_us);
          if (latency > c_.deadline_us) ++deadline_misses_;
          if (observer != nullptr) observer->record_work_unit_completion("frame:" + std::to_string(task.frame), completion.end_us);
          if (c_.dma_double_buffering == 0 && task.frame + 1 < c_.frames
              && (observer == nullptr || observer->includes_work_unit("frame:" + std::to_string(task.frame + 1)))) {
            const double next_release = frame_arrivals_us_[static_cast<std::size_t>(task.frame + 1)];
            enqueue_future(future, make_runtime_task(task.frame + 1, initial_stage, next_release, std::max(next_release, completion.end_us)), next_enqueue_sequence);
          }
        } else if (task.stage == RuntimeStage::Control) {
          ++control_tasks_;
          const double response = completion.end_us - task.release_us;
          control_response_times_us_.push_back(response);
          if (response > c_.control_deadline_us) ++control_deadline_misses_;
          if (observer != nullptr) observer->record_work_unit_completion("control:" + std::to_string(task.control_sequence), completion.end_us);
        } else {
          enqueue_future(future, make_runtime_task(task.frame, static_cast<RuntimeStage>(static_cast<int>(task.stage) + 1), task.release_us, completion.end_us), next_enqueue_sequence);
        }
      }
      if (observer != nullptr && !observer->is_running()) break;
      for (std::size_t index = 0; index < pending.size(); ++index) {
        if (busy[index] || pending[index].empty()) continue;
        auto& queue = pending[index];
        const auto selected = std::min_element(queue.begin(), queue.end(), [this](const RuntimeTask& left, const RuntimeTask& right) {
          return resource_task_precedes(left, right);
        });
        RuntimeTask task = *selected;
        const double end = time + task.duration_us;
        const auto task_energy = runtime_task_energy(task);
        if (!observe_study_dispatch(observer, task, time, end, resource_name(task.resource), task_energy)) break;
        queue.erase(selected);
        busy[index] = true;
        if (task.frame >= 0) vision_resource_queue_delays_us_.push_back(time - task.ready_us);
        if ((task.stage == RuntimeStage::Camera || task.stage == RuntimeStage::VectorKernel) && task.vector_offset == 0) {
          vision_start_delays_us_.push_back(time - task.release_us);
        }
        trace_.push_back({task.frame, task.name, resource_name(task.resource), time, end});
        accumulate_runtime_energy(task, task_energy);
        const auto resource = task.resource;
        enqueue_completion(completions, end, resource, std::move(task), next_completion_sequence);
        if (observer != nullptr && !observer->is_running()) break;
      }
      if (observer != nullptr && !observer->is_running()) break;
      if (observer != nullptr && !has_same_time_events(time, future, completions)) {
        const auto snapshot = study_queue_snapshot(time, pending, completions);
        if (!observer->finish_quiescent_timestamp(time, snapshot)) break;
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
  std::uint64_t profile_first_frame_transfer_bytes_ = 0;
  std::uint64_t profile_steady_state_transfer_bytes_ = 0;
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

}  // namespace axissim

