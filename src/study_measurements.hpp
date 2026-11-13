#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "metrics.hpp"

namespace axissim {

enum class StudyMeasurementMode { Finite, Sustained };
enum class StudyExecutionStatus { Running, Complete, Invalid, Censored, Failed };
enum class StudyCensorReason { None, MaxTaskEvents, MaxSimulatedTime, MaxDrainSimulatedTime, MaxBacklogWorkUnits };
enum class StudyWorkUnitKind { Frame, Control };

struct StudyMeasurementLimits {
  std::uint64_t max_task_events = 0;
  double max_simulated_time_us = 0.0;
  double max_drain_simulated_us = 0.0;
  std::uint64_t max_backlog_work_units = 0;
};

struct StudyMeasurementRequest {
  StudyMeasurementMode mode = StudyMeasurementMode::Finite;
  double window_start_us = 0.0;
  double window_end_us = 0.0;
  StudyMeasurementLimits limits;
};

struct StudyWorkUnitSpec {
  std::string id;
  StudyWorkUnitKind kind = StudyWorkUnitKind::Frame;
  std::uint64_t sequence = 0;
  double release_us = 0.0;
  double relative_deadline_us = 0.0;
};

struct StudyWorkUnitRecord {
  StudyWorkUnitSpec spec;
  bool target_cohort = false;
  bool released = false;
  std::optional<double> completion_us;
};

struct StudyTaskEnergyPj {
  double vector = 0.0;
  double npu_arithmetic = 0.0;
  double dram = 0.0;
  double dma = 0.0;
  double total = 0.0;
};

struct StudyTaskIntervalRecord {
  std::uint64_t task_id = 0;
  std::string work_unit_id;
  std::string task_name;
  std::string resource;
  double release_us = 0.0;
  double ready_us = 0.0;
  double start_us = 0.0;
  double end_us = 0.0;
  double observed_end_us = 0.0;
  std::string completion_status = "in_flight_at_censor";
  bool completion_observed = false;
  double vector_energy_j = 0.0;
  double npu_arithmetic_energy_j = 0.0;
  double dram_energy_j = 0.0;
  double dma_energy_j = 0.0;
  double total_dynamic_energy_j = 0.0;
};

struct StudyQueueSnapshot {
  double timestamp_us = 0.0;
  std::array<std::uint64_t, 3> ready_by_resource{};
  std::array<std::uint64_t, 3> in_flight_by_resource{};
  std::optional<std::uint64_t> dependency_blocked_tasks;
  std::uint64_t scheduled_not_released_work_units = 0;
  std::uint64_t released_unfinished_work_units = 0;
};

template <typename T>
struct StudyMetric {
  std::optional<T> value;
  std::string null_reason;
};

struct StudyCohortMetrics {
  std::uint64_t scheduled = 0;
  std::uint64_t released = 0;
  std::uint64_t completed_in_window = 0;
  std::uint64_t completed_by_drain = 0;
  std::uint64_t incomplete = 0;
  StudyMetric<double> offered_rate_per_s;
  StudyMetric<double> mean_latency_us;
  StudyMetric<double> p99_latency_us;
  StudyMetric<std::uint64_t> deadline_misses;
  StudyMetric<double> deadline_miss_rate;
};

struct StudyEnergyMetrics {
  StudyMetric<double> dynamic_energy_j;
  StudyMetric<double> vector_energy_j;
  StudyMetric<double> npu_arithmetic_energy_j;
  StudyMetric<double> dram_energy_j;
  StudyMetric<double> dma_energy_j;
  StudyMetric<double> static_energy_j;
  StudyMetric<double> it_energy_j;
  StudyMetric<double> average_it_power_w;
  StudyMetric<double> energy_per_completed_work_unit_j;
};

struct StudyMeasurementResult {
  StudyMeasurementMode mode = StudyMeasurementMode::Finite;
  StudyExecutionStatus execution_status = StudyExecutionStatus::Running;
  StudyCensorReason censor_reason = StudyCensorReason::None;
  std::string error_code;
  std::string operating_state = "not_applicable";
  std::uint64_t task_event_count = 0;
  double last_model_time_us = 0.0;
  double interval_start_us = 0.0;
  double interval_end_us = 0.0;
  double interval_duration_us = 0.0;
  bool interval_complete = false;
  bool target_cohort_complete = false;
  StudyMetric<double> completed_throughput_per_s;
  StudyMetric<double> offered_throughput_per_s;
  StudyEnergyMetrics energy;
  StudyCohortMetrics frame_cohort;
  StudyCohortMetrics control_cohort;
  std::uint64_t scheduled_frames = 0;
  std::uint64_t scheduled_controls = 0;
  std::uint64_t released_frames = 0;
  std::uint64_t released_controls = 0;
  std::uint64_t completed_frames_by_drain = 0;
  std::uint64_t completed_controls_by_drain = 0;
  std::uint64_t completed_frames_in_window = 0;
  std::uint64_t completed_controls_in_window = 0;
  std::uint64_t final_unfinished_work_units = 0;
  std::uint64_t final_scheduled_not_released_work_units = 0;
  std::optional<StudyQueueSnapshot> start_snapshot;
  std::optional<StudyQueueSnapshot> end_snapshot;
  std::optional<std::int64_t> backlog_delta_work_units;
  std::optional<bool> power_budget_feasible;
  std::vector<StudyWorkUnitRecord> work_units;
  std::vector<StudyTaskIntervalRecord> task_intervals;
};

[[nodiscard]] inline double study_interval_overlap_energy_j(double energy_j, double task_start_us, double task_end_us,
                                                             double interval_start_us, double interval_end_us) {
  if (!std::isfinite(energy_j) || energy_j < 0.0 || !std::isfinite(task_start_us) || !std::isfinite(task_end_us)
      || !std::isfinite(interval_start_us) || !std::isfinite(interval_end_us)
      || task_end_us < task_start_us || interval_end_us < interval_start_us) {
    throw std::invalid_argument("Study task energy and interval times must be finite and durations non-negative");
  }
  // Dispatch-attributed energy is prorated uniformly over task duration. This is
  // an accounting approximation, not an instantaneous activity/power trace.
  if (task_end_us == task_start_us) {
    return task_start_us >= interval_start_us && task_start_us < interval_end_us ? energy_j : 0.0;
  }
  const double overlap_start = std::max(task_start_us, interval_start_us);
  const double overlap_end = std::min(task_end_us, interval_end_us);
  if (overlap_end <= overlap_start) return 0.0;
  return energy_j * ((overlap_end - overlap_start) / (task_end_us - task_start_us));
}

[[nodiscard]] inline double study_energy_conservation_tolerance_j(double combined_energy_j) {
  return std::max(1e-12, 1e-12 * std::abs(combined_energy_j));
}

[[nodiscard]] inline double study_percentile_linear(std::vector<double> values, double percentile) {
  if (values.empty() || !std::isfinite(percentile) || percentile < 0.0 || percentile > 1.0) {
    throw std::invalid_argument("Percentile requires samples and a finite value in [0,1]");
  }
  std::sort(values.begin(), values.end());
  const double rank = static_cast<double>(values.size() - 1) * percentile;
  const auto low = static_cast<std::size_t>(std::floor(rank));
  const auto high = static_cast<std::size_t>(std::ceil(rank));
  return values[low] + (values[high] - values[low]) * (rank - static_cast<double>(low));
}

// Optional study-only observer. It observes events already chosen by the production
// schedulers and never schedules work or changes resource arbitration.
class StudyMeasurementObserver {
 public:
  StudyMeasurementObserver(StudyMeasurementRequest request, std::vector<StudyWorkUnitSpec> schedule)
      : request_(request) {
    validate_request();
    std::map<std::string, bool> seen_ids;
    for (auto& spec : schedule) {
      const std::string expected_id = spec.kind == StudyWorkUnitKind::Frame
          ? "frame:" + std::to_string(spec.sequence) : "control:" + std::to_string(spec.sequence);
      if (spec.id != expected_id || !std::isfinite(spec.release_us) || spec.release_us < 0.0
          || !std::isfinite(spec.relative_deadline_us) || spec.relative_deadline_us < 0.0) {
        throw std::invalid_argument("Study work-unit identity, release, or deadline is invalid");
      }
      if (!seen_ids.emplace(spec.id, true).second) throw std::invalid_argument("Study work-unit identities must be unique");
      if (request_.mode == StudyMeasurementMode::Sustained && spec.release_us >= request_.window_end_us) continue;
      const bool target = request_.mode == StudyMeasurementMode::Finite
          || (spec.release_us >= request_.window_start_us && spec.release_us < request_.window_end_us);
      units_.push_back({std::move(spec), target, false, std::nullopt});
    }
    std::stable_sort(units_.begin(), units_.end(), [](const StudyWorkUnitRecord& left, const StudyWorkUnitRecord& right) {
      if (left.spec.release_us != right.spec.release_us) return left.spec.release_us < right.spec.release_us;
      if (left.spec.kind != right.spec.kind) return left.spec.kind == StudyWorkUnitKind::Frame;
      return left.spec.sequence < right.spec.sequence;
    });
    for (std::size_t i = 0; i < units_.size(); ++i) unit_index_.emplace(units_[i].spec.id, i);
    result_.mode = request_.mode;
    result_.interval_start_us = request_.mode == StudyMeasurementMode::Finite ? 0.0 : request_.window_start_us;
    result_.interval_end_us = request_.mode == StudyMeasurementMode::Sustained ? request_.window_end_us : 0.0;
    result_.interval_duration_us = request_.mode == StudyMeasurementMode::Sustained
        ? request_.window_end_us - request_.window_start_us : 0.0;
    result_.work_units = units_;
    if (units_.empty()) set_invalid("empty_workload_unsupported");
    else if (request_.mode == StudyMeasurementMode::Sustained && target_unit_count() == 0) set_invalid("empty_target_cohort_unsupported");
  }

  [[nodiscard]] static StudyMeasurementResult invalid_result(StudyMeasurementMode mode, std::string error_code) {
    StudyMeasurementResult result;
    result.mode = mode;
    result.execution_status = StudyExecutionStatus::Invalid;
    result.error_code = std::move(error_code);
    return result;
  }

  [[nodiscard]] bool is_running() const { return result_.execution_status == StudyExecutionStatus::Running; }
  [[nodiscard]] bool is_censored() const { return result_.execution_status == StudyExecutionStatus::Censored; }
  [[nodiscard]] bool is_invalid() const { return result_.execution_status == StudyExecutionStatus::Invalid; }
  [[nodiscard]] std::uint64_t task_event_count() const { return task_event_count_; }
  [[nodiscard]] double last_model_time_us() const { return result_.last_model_time_us; }
  [[nodiscard]] bool window_frozen() const { return window_frozen_; }

  [[nodiscard]] bool all_units_complete() const {
    return !units_.empty() && std::all_of(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) {
      return unit.released && unit.completion_us.has_value();
    });
  }
  [[nodiscard]] bool has_unfinished_units() const { return !all_units_complete(); }
  [[nodiscard]] std::uint64_t scheduled_frame_count() const { return count_kind(StudyWorkUnitKind::Frame); }
  [[nodiscard]] std::uint64_t scheduled_control_count() const { return count_kind(StudyWorkUnitKind::Control); }
  [[nodiscard]] bool includes_work_unit(const std::string& id) const { return unit_index_.contains(id); }
  void fail_if_unterminated(std::string code) {
    if (is_running()) fail(std::move(code), result_.last_model_time_us);
  }

  [[nodiscard]] double next_release_time_us() const {
    for (const auto& unit : units_) if (!unit.released) return unit.spec.release_us;
    return std::numeric_limits<double>::infinity();
  }

  [[nodiscard]] double next_observer_time_us() const {
    if (!is_running()) return std::numeric_limits<double>::infinity();
    double next = next_release_time_us();
    if (!start_boundary_reached_) next = std::min(next, request_.mode == StudyMeasurementMode::Finite ? 0.0 : request_.window_start_us);
    if (request_.mode == StudyMeasurementMode::Sustained && !window_frozen_) next = std::min(next, request_.window_end_us);
    if (has_unfinished_units() || (request_.mode == StudyMeasurementMode::Sustained && !window_frozen_)) {
      next = std::min(next, request_.limits.max_simulated_time_us);
    }
    if (request_.mode == StudyMeasurementMode::Sustained && window_frozen_ && has_unfinished_units()) {
      next = std::min(next, request_.window_end_us + request_.limits.max_drain_simulated_us);
    }
    return next;
  }

  [[nodiscard]] bool process_releases_through(double time_us) {
    if (!is_running()) return false;
    for (auto& unit : units_) {
      if (unit.released || unit.spec.release_us > time_us) continue;
      if (!consume_scheduler_event(time_us)) return false;
      unit.released = true;
      if (backlog_at(time_us) > request_.limits.max_backlog_work_units) {
        censor(StudyCensorReason::MaxBacklogWorkUnits, time_us);
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool record_ready_event(double time_us) { return consume_scheduler_event(time_us); }
  [[nodiscard]] bool record_task_completion_event(double time_us) { return consume_scheduler_event(time_us); }

  void record_task_completed(const std::string& work_unit_id, const std::string& task_name, double end_us) {
    const auto found = std::find_if(tasks_.begin(), tasks_.end(), [&work_unit_id,&task_name,end_us](const StudyTaskIntervalRecord& task) {
      return !task.completion_observed && task.work_unit_id == work_unit_id && task.task_name == task_name
          && task.end_us == end_us;
    });
    if (found != tasks_.end()) {
      found->completion_observed = true;
      found->observed_end_us = end_us;
      found->completion_status = "completed";
    }
  }

  [[nodiscard]] bool record_task_dispatch(double time_us, std::string work_unit_id, std::string task_name,
                                          std::string resource, double release_us, double ready_us,
                                          double start_us, double end_us,
                                          const StudyTaskEnergyPj& energy_pj) {
    if (!consume_scheduler_event(time_us)) return false;
    const auto found = unit_index_.find(work_unit_id);
    if (found == unit_index_.end() || !units_[found->second].released) {
      fail("study_task_without_released_work_unit", time_us);
      return false;
    }
    if (!std::isfinite(release_us) || !std::isfinite(ready_us) || release_us < 0.0 || ready_us < release_us
        || !std::isfinite(start_us) || !std::isfinite(end_us) || start_us < ready_us || end_us < start_us || start_us != time_us
        || !std::isfinite(energy_pj.vector) || energy_pj.vector < 0.0
        || !std::isfinite(energy_pj.npu_arithmetic) || energy_pj.npu_arithmetic < 0.0
        || !std::isfinite(energy_pj.dram) || energy_pj.dram < 0.0
        || !std::isfinite(energy_pj.dma) || energy_pj.dma < 0.0
        || !std::isfinite(energy_pj.total) || energy_pj.total < 0.0) {
      fail("study_task_interval_invalid", time_us);
      return false;
    }
    const std::uint64_t task_id = next_task_id_++;
    tasks_.push_back({task_id, std::move(work_unit_id), std::move(task_name), std::move(resource), release_us, ready_us, start_us, end_us, start_us, "in_flight_at_censor", false,
        energy_pj.vector / 1e12, energy_pj.npu_arithmetic / 1e12, energy_pj.dram / 1e12,
        energy_pj.dma / 1e12, energy_pj.total / 1e12});
    task_energy_pj_.push_back({start_us, end_us, energy_pj});
    return true;
  }

  // Compatibility overload for focused observer tests and callers that do not
  // have scheduler-ready timestamps. Production scheduler hooks use the full
  // overload above and preserve the observed ready time.
  [[nodiscard]] bool record_task_dispatch(double time_us, std::string work_unit_id, std::string task_name,
                                          std::string resource, double start_us, double end_us,
                                          const StudyTaskEnergyPj& energy_pj) {
    const auto found = unit_index_.find(work_unit_id);
    if (found == unit_index_.end()) return false;
    const double release_us = units_[found->second].spec.release_us;
    return record_task_dispatch(time_us,std::move(work_unit_id),std::move(task_name),std::move(resource),
                                release_us,start_us,start_us,end_us,energy_pj);
  }

  void record_work_unit_completion(const std::string& id, double completion_us) {
    const auto found = unit_index_.find(id);
    if (found == unit_index_.end()) { fail("study_completion_without_work_unit", completion_us); return; }
    auto& unit = units_[found->second];
    if (!unit.released || unit.completion_us || completion_us < unit.spec.release_us || !std::isfinite(completion_us)) {
      fail("study_work_unit_completion_invalid", completion_us);
      return;
    }
    unit.completion_us = completion_us;
  }

  [[nodiscard]] bool finish_quiescent_timestamp(double time_us, StudyQueueSnapshot snapshot) {
    if (!is_running()) return false;
    result_.last_model_time_us = std::max(result_.last_model_time_us, time_us);
    if (request_.mode == StudyMeasurementMode::Finite && time_us == 0.0 && !result_.start_snapshot) {
      result_.start_snapshot = make_snapshot(time_us, std::move(snapshot));
      start_boundary_reached_ = true;
    }
    if (request_.mode == StudyMeasurementMode::Sustained) {
      if (time_us == request_.window_start_us && !result_.start_snapshot) {
        result_.start_snapshot = make_snapshot(time_us, snapshot);
        start_boundary_reached_ = true;
      }
      if (time_us == request_.window_end_us && !window_frozen_) {
        result_.end_snapshot = make_snapshot(time_us, std::move(snapshot));
        freeze_window_energy();
      }
    }

    if (backlog_at(time_us) > request_.limits.max_backlog_work_units) {
      censor(StudyCensorReason::MaxBacklogWorkUnits, time_us);
      return false;
    }
    if (time_us == request_.limits.max_simulated_time_us
        && (has_unfinished_units() || (request_.mode == StudyMeasurementMode::Sustained && !window_frozen_))) {
      censor(StudyCensorReason::MaxSimulatedTime, time_us);
      return false;
    }
    if (request_.mode == StudyMeasurementMode::Sustained
        && time_us == request_.window_end_us + request_.limits.max_drain_simulated_us && has_unfinished_units()) {
      censor(StudyCensorReason::MaxDrainSimulatedTime, time_us);
      return false;
    }
    if (all_units_complete() && (request_.mode == StudyMeasurementMode::Finite || window_frozen_)) {
      result_.execution_status = StudyExecutionStatus::Complete;
      if (request_.mode == StudyMeasurementMode::Finite) {
        result_.interval_end_us = 0.0;
        for (const auto& unit : units_) result_.interval_end_us = std::max(result_.interval_end_us, unit.completion_us.value_or(0.0));
        result_.interval_duration_us = result_.interval_end_us;
        result_.interval_complete = true;
      }
    }
    return true;
  }

  void censor(StudyCensorReason reason, double time_us) {
    if (!is_running()) return;
    result_.execution_status = StudyExecutionStatus::Censored;
    result_.censor_reason = reason;
    result_.last_model_time_us = std::max(result_.last_model_time_us, time_us);
    if (request_.mode == StudyMeasurementMode::Finite) {
      result_.interval_end_us = time_us;
      result_.interval_duration_us = time_us;
    }
    result_.interval_complete = request_.mode == StudyMeasurementMode::Sustained && window_frozen_;
  }

  [[nodiscard]] StudyMeasurementResult finish(const Config& config, double whole_run_dynamic_energy_pj) {
    sync_result_counts();
    result_.task_event_count = task_event_count_;
    result_.work_units = units_;
    for (auto& task : tasks_) if (!task.completion_observed) task.observed_end_us = std::min(task.end_us, result_.last_model_time_us);
    result_.task_intervals = tasks_;
    result_.final_unfinished_work_units = count_unfinished_units();
    result_.final_scheduled_not_released_work_units = static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return !unit.released; }));
    if (result_.execution_status == StudyExecutionStatus::Invalid || result_.execution_status == StudyExecutionStatus::Failed) return result_;

    if (request_.mode == StudyMeasurementMode::Sustained) {
      result_.interval_start_us = request_.window_start_us;
      result_.interval_end_us = request_.window_end_us;
      result_.interval_duration_us = request_.window_end_us - request_.window_start_us;
      result_.interval_complete = window_frozen_;
      if (result_.start_snapshot && result_.end_snapshot) {
        result_.backlog_delta_work_units = static_cast<std::int64_t>(result_.end_snapshot->released_unfinished_work_units)
            - static_cast<std::int64_t>(result_.start_snapshot->released_unfinished_work_units);
        // This fixed-window label is not a claim of long-run queue stability.
        result_.operating_state = *result_.backlog_delta_work_units > 0
            ? "backlog_growing" : "no_growth_observed";
      }
    } else if (result_.execution_status != StudyExecutionStatus::Complete && result_.interval_end_us == 0.0) {
      result_.interval_end_us = result_.last_model_time_us;
      result_.interval_duration_us = result_.interval_end_us;
    }

    const bool metrics_available = request_.mode == StudyMeasurementMode::Finite
        ? result_.execution_status == StudyExecutionStatus::Complete : window_frozen_;
    if (!metrics_available) {
      const std::string reason = "measurement_window_incomplete";
      set_energy_unavailable(reason);
      result_.completed_throughput_per_s.null_reason = reason;
      result_.offered_throughput_per_s.null_reason = reason;
    } else {
      const double duration_us = result_.interval_duration_us;
      const double dynamic_energy_pj = request_.mode == StudyMeasurementMode::Finite ? whole_run_dynamic_energy_pj : window_dynamic_energy_pj_;
      const ModelAccounting accounting = model_accounting(config, dynamic_energy_pj, duration_us);
      if (config.it_power_budget_w > 0.0) result_.power_budget_feasible = accounting.power_feasible;
      set_value(result_.energy.dynamic_energy_j, dynamic_energy_pj / 1e12);
      set_energy_categories();
      set_value(result_.energy.static_energy_j, accounting.static_energy_pj / 1e12);
      set_value(result_.energy.it_energy_j, accounting.it_energy_pj / 1e12);
      if (duration_us > 0.0) set_value(result_.energy.average_it_power_w, accounting.average_it_power_w);
      else result_.energy.average_it_power_w.null_reason = "zero_duration_interval";

      const std::uint64_t completed = request_.mode == StudyMeasurementMode::Finite
          ? result_.completed_frames_by_drain + result_.completed_controls_by_drain : completed_in_window_total();
      if (completed > 0) set_value(result_.energy.energy_per_completed_work_unit_j,
          (accounting.it_energy_pj / 1e12) / static_cast<double>(completed));
      else result_.energy.energy_per_completed_work_unit_j.null_reason = "no_completed_work_units";
      if (duration_us > 0.0) {
        set_value(result_.completed_throughput_per_s, static_cast<double>(completed) * 1'000'000.0 / duration_us);
        if (request_.mode == StudyMeasurementMode::Sustained) {
          set_value(result_.offered_throughput_per_s, static_cast<double>(target_unit_count()) * 1'000'000.0 / duration_us);
        } else result_.offered_throughput_per_s.null_reason = "not_applicable_finite_protocol";
      } else {
        result_.completed_throughput_per_s.null_reason = "zero_duration_interval";
        result_.offered_throughput_per_s.null_reason = request_.mode == StudyMeasurementMode::Finite ? "not_applicable_finite_protocol" : "zero_duration_interval";
      }
    }

    finalize_cohort(StudyWorkUnitKind::Frame, result_.frame_cohort);
    finalize_cohort(StudyWorkUnitKind::Control, result_.control_cohort);
    result_.target_cohort_complete = result_.frame_cohort.incomplete == 0 && result_.control_cohort.incomplete == 0
        && result_.frame_cohort.released + result_.control_cohort.released == target_unit_count();
    if (request_.mode == StudyMeasurementMode::Finite && result_.execution_status == StudyExecutionStatus::Complete) result_.target_cohort_complete = true;
    return result_;
  }

 private:
  struct TaskEnergyInterval { double start_us; double end_us; StudyTaskEnergyPj energy; };

  void validate_request() {
    const auto& limits = request_.limits;
    if (limits.max_task_events == 0 || limits.max_backlog_work_units == 0
        || !std::isfinite(limits.max_simulated_time_us) || limits.max_simulated_time_us <= 0.0
        || !std::isfinite(limits.max_drain_simulated_us) || limits.max_drain_simulated_us <= 0.0) {
      throw std::invalid_argument("Study event, model-time, and drain limits must be positive");
    }
    if (request_.mode == StudyMeasurementMode::Sustained
        && (!std::isfinite(request_.window_start_us) || !std::isfinite(request_.window_end_us)
            || request_.window_start_us < 0.0 || request_.window_end_us <= request_.window_start_us
            || request_.window_end_us > limits.max_simulated_time_us)) {
      throw std::invalid_argument("Sustained study window must satisfy 0 <= t0 < t1 <= max simulated time");
    }
  }

  void set_invalid(std::string code) {
    result_.execution_status = StudyExecutionStatus::Invalid;
    result_.error_code = std::move(code);
  }
  [[nodiscard]] std::uint64_t count_kind(StudyWorkUnitKind kind) const {
    return static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [kind](const StudyWorkUnitRecord& unit) { return unit.spec.kind == kind; }));
  }
  [[nodiscard]] std::size_t target_unit_count() const {
    return static_cast<std::size_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return unit.target_cohort; }));
  }
  [[nodiscard]] bool consume_scheduler_event(double time_us) {
    if (!is_running()) return false;
    if (task_event_count_ >= request_.limits.max_task_events) {
      censor(StudyCensorReason::MaxTaskEvents, time_us);
      return false;
    }
    ++task_event_count_;
    return true;
  }
  void fail(std::string code, double time_us) {
    if (!is_running()) return;
    result_.execution_status = StudyExecutionStatus::Failed;
    result_.error_code = std::move(code);
    result_.last_model_time_us = std::max(result_.last_model_time_us, time_us);
  }
  [[nodiscard]] std::uint64_t backlog_at(double time_us) const {
    return static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [time_us](const StudyWorkUnitRecord& unit) {
      return unit.released && unit.spec.release_us <= time_us && (!unit.completion_us || *unit.completion_us > time_us);
    }));
  }
  [[nodiscard]] std::uint64_t count_unfinished_units() const {
    return static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return unit.released && !unit.completion_us; }));
  }
  [[nodiscard]] StudyQueueSnapshot make_snapshot(double time_us, StudyQueueSnapshot snapshot) const {
    snapshot.timestamp_us = time_us;
    snapshot.released_unfinished_work_units = backlog_at(time_us);
    snapshot.scheduled_not_released_work_units = static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [time_us](const StudyWorkUnitRecord& unit) {
      return !unit.released && unit.spec.release_us > time_us;
    }));
    return snapshot;
  }
  void freeze_window_energy() {
    window_dynamic_energy_pj_ = interval_energy_pj(request_.window_start_us, request_.window_end_us).total;
    window_frozen_ = true;
    result_.interval_complete = true;
  }
  [[nodiscard]] StudyTaskEnergyPj interval_energy_pj(double start_us, double end_us) const {
    StudyTaskEnergyPj total;
    for (const auto& task : task_energy_pj_) {
      const auto overlap = [start_us, end_us, &task](double energy) {
        return study_interval_overlap_energy_j(energy / 1e12, task.start_us, task.end_us, start_us, end_us) * 1e12;
      };
      total.vector += overlap(task.energy.vector);
      total.npu_arithmetic += overlap(task.energy.npu_arithmetic);
      total.dram += overlap(task.energy.dram);
      total.dma += overlap(task.energy.dma);
      total.total += overlap(task.energy.total);
    }
    return total;
  }
  void set_energy_unavailable(const std::string& reason) {
    result_.energy.dynamic_energy_j.null_reason = reason;
    result_.energy.vector_energy_j.null_reason = reason;
    result_.energy.npu_arithmetic_energy_j.null_reason = reason;
    result_.energy.dram_energy_j.null_reason = reason;
    result_.energy.dma_energy_j.null_reason = reason;
    result_.energy.static_energy_j.null_reason = reason;
    result_.energy.it_energy_j.null_reason = reason;
    result_.energy.average_it_power_w.null_reason = reason;
    result_.energy.energy_per_completed_work_unit_j.null_reason = reason;
  }
  static void set_value(StudyMetric<double>& metric, double value) { metric.value = value; metric.null_reason.clear(); }
  void set_energy_categories() {
    StudyTaskEnergyPj categories;
    if (request_.mode == StudyMeasurementMode::Sustained) categories = interval_energy_pj(request_.window_start_us, request_.window_end_us);
    else for (const auto& task : task_energy_pj_) {
      categories.vector += task.energy.vector;
      categories.npu_arithmetic += task.energy.npu_arithmetic;
      categories.dram += task.energy.dram;
      categories.dma += task.energy.dma;
    }
    set_value(result_.energy.vector_energy_j, categories.vector / 1e12);
    set_value(result_.energy.npu_arithmetic_energy_j, categories.npu_arithmetic / 1e12);
    set_value(result_.energy.dram_energy_j, categories.dram / 1e12);
    set_value(result_.energy.dma_energy_j, categories.dma / 1e12);
  }
  [[nodiscard]] std::uint64_t count_completed_in_window(StudyWorkUnitKind kind, bool target_only = false) const {
    if (request_.mode == StudyMeasurementMode::Finite) {
      return static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [kind, target_only](const StudyWorkUnitRecord& unit) {
        return unit.spec.kind == kind && (!target_only || unit.target_cohort) && unit.completion_us.has_value();
      }));
    }
    return static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [this, kind, target_only](const StudyWorkUnitRecord& unit) {
      return unit.spec.kind == kind && (!target_only || unit.target_cohort) && unit.completion_us
          && *unit.completion_us >= request_.window_start_us && *unit.completion_us < request_.window_end_us;
    }));
  }
  [[nodiscard]] std::uint64_t completed_in_window_total() const {
    return result_.completed_frames_in_window + result_.completed_controls_in_window;
  }
  void sync_result_counts() {
    result_.scheduled_frames = count_kind(StudyWorkUnitKind::Frame);
    result_.scheduled_controls = count_kind(StudyWorkUnitKind::Control);
    result_.released_frames = static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return unit.spec.kind == StudyWorkUnitKind::Frame && unit.released; }));
    result_.released_controls = static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return unit.spec.kind == StudyWorkUnitKind::Control && unit.released; }));
    result_.completed_frames_by_drain = static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return unit.spec.kind == StudyWorkUnitKind::Frame && unit.completion_us; }));
    result_.completed_controls_by_drain = static_cast<std::uint64_t>(std::count_if(units_.cbegin(), units_.cend(), [](const StudyWorkUnitRecord& unit) { return unit.spec.kind == StudyWorkUnitKind::Control && unit.completion_us; }));
    result_.completed_frames_in_window = count_completed_in_window(StudyWorkUnitKind::Frame);
    result_.completed_controls_in_window = count_completed_in_window(StudyWorkUnitKind::Control);
    result_.frame_cohort.completed_in_window = count_completed_in_window(StudyWorkUnitKind::Frame, true);
    result_.control_cohort.completed_in_window = count_completed_in_window(StudyWorkUnitKind::Control, true);
  }
  void finalize_cohort(StudyWorkUnitKind kind, StudyCohortMetrics& metrics) {
    std::vector<double> latencies;
    std::uint64_t scheduled = 0;
    std::uint64_t released = 0;
    std::uint64_t completed = 0;
    std::uint64_t misses = 0;
    for (const auto& unit : units_) {
      if (unit.spec.kind != kind || !unit.target_cohort) continue;
      ++scheduled;
      if (unit.released) ++released;
      if (unit.completion_us) {
        ++completed;
        const double latency = *unit.completion_us - unit.spec.release_us;
        latencies.push_back(latency);
        if (latency > unit.spec.relative_deadline_us) ++misses;
      }
    }
    metrics.scheduled = scheduled;
    metrics.released = released;
    metrics.completed_by_drain = completed;
    metrics.incomplete = scheduled - completed;
    const bool complete = scheduled > 0 && released == scheduled && completed == scheduled;
    if (scheduled == 0) {
      metrics.offered_rate_per_s.null_reason = "no_target_work_units";
      metrics.mean_latency_us.null_reason = "no_target_work_units";
      metrics.p99_latency_us.null_reason = "no_target_work_units";
      metrics.deadline_misses.null_reason = "no_target_work_units";
      metrics.deadline_miss_rate.null_reason = "no_target_work_units";
    } else {
      if (request_.mode == StudyMeasurementMode::Sustained && window_frozen_) {
        set_value(metrics.offered_rate_per_s, static_cast<double>(scheduled) * 1'000'000.0 / (request_.window_end_us - request_.window_start_us));
      } else metrics.offered_rate_per_s.null_reason = request_.mode == StudyMeasurementMode::Finite ? "not_applicable_finite_protocol" : "measurement_window_incomplete";
      if (!complete) {
        const std::string reason = result_.execution_status == StudyExecutionStatus::Censored ? "cohort_incomplete_after_censor" : "cohort_incomplete";
        metrics.mean_latency_us.null_reason = reason;
        metrics.p99_latency_us.null_reason = reason;
        metrics.deadline_misses.null_reason = reason;
        metrics.deadline_miss_rate.null_reason = reason;
      } else {
        set_value(metrics.mean_latency_us, std::accumulate(latencies.cbegin(), latencies.cend(), 0.0) / static_cast<double>(latencies.size()));
        set_value(metrics.p99_latency_us, study_percentile_linear(std::move(latencies), 0.99));
        metrics.deadline_misses.value = misses;
        metrics.deadline_misses.null_reason.clear();
        set_value(metrics.deadline_miss_rate, static_cast<double>(misses) / static_cast<double>(scheduled));
      }
    }
  }

  StudyMeasurementRequest request_;
  StudyMeasurementResult result_;
  std::vector<StudyWorkUnitRecord> units_;
  std::map<std::string, std::size_t> unit_index_;
  std::vector<TaskEnergyInterval> task_energy_pj_;
  std::vector<StudyTaskIntervalRecord> tasks_;
  std::uint64_t task_event_count_ = 0;
  std::uint64_t next_task_id_ = 0;
  double window_dynamic_energy_pj_ = 0.0;
  bool start_boundary_reached_ = false;
  bool window_frozen_ = false;
};

}  // namespace axissim
