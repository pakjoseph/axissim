#include "simulation.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef AXISSIM_SOURCE_DIR
#error AXISSIM_SOURCE_DIR must be defined for study measurement tests
#endif

namespace {

using namespace axissim;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void near(double actual, double expected, double tolerance, const std::string& message) {
  check(std::abs(actual - expected) <= tolerance, message + " (actual=" + std::to_string(actual)
        + ", expected=" + std::to_string(expected) + ")");
}

StudyMeasurementLimits limits(std::uint64_t events = 1'000'000, double simulated_us = 1'000'000.0,
                              double drain_us = 10'000.0, std::uint64_t backlog = 100'000) {
  return {events, simulated_us, drain_us, backlog};
}

StudyTaskEnergyPj energy(double vector_j = 0.0, double npu_j = 0.0, double dram_j = 0.0, double dma_j = 0.0) {
  return {vector_j * 1e12, npu_j * 1e12, dram_j * 1e12, dma_j * 1e12,
          (vector_j + npu_j + dram_j + dma_j) * 1e12};
}

StudyWorkUnitSpec frame(std::uint64_t sequence, double release_us, double deadline_us = 100.0) {
  return {"frame:" + std::to_string(sequence), StudyWorkUnitKind::Frame, sequence, release_us, deadline_us};
}

StudyQueueSnapshot snapshot(double time_us) {
  StudyQueueSnapshot result;
  result.timestamp_us = time_us;
  return result;
}

StudyMeasurementRequest finite_request(StudyMeasurementLimits caps = limits()) {
  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Finite;
  request.limits = caps;
  return request;
}

void complete_unit(StudyMeasurementObserver& observer, std::uint64_t sequence, double time_us) {
  check(observer.record_task_completion_event(time_us), "completion event is admitted");
  observer.record_work_unit_completion("frame:" + std::to_string(sequence), time_us);
}

void test_overlap_and_energy_partition_boundaries() {
  near(study_interval_overlap_energy_j(12.0, 2.0, 8.0, 0.0, 5.0), 6.0, 1e-12, "first half-open interval receives proportional energy");
  near(study_interval_overlap_energy_j(12.0, 2.0, 8.0, 5.0, 10.0), 6.0, 1e-12, "adjacent interval receives the remaining energy");
  near(study_interval_overlap_energy_j(10.0, 0.0, 10.0, 0.0, 4.0), 4.0, 1e-12, "leading interval energy is proportional");
  near(study_interval_overlap_energy_j(10.0, 0.0, 10.0, 4.0, 10.0), 6.0, 1e-12, "trailing interval energy is proportional");
  near(study_interval_overlap_energy_j(3.0, 4.0, 4.0, 0.0, 4.0), 0.0, 0.0, "zero-duration energy at a right boundary is excluded");
  near(study_interval_overlap_energy_j(3.0, 4.0, 4.0, 4.0, 10.0), 3.0, 0.0, "zero-duration energy belongs to the window starting at its timestamp");
  near(study_interval_overlap_energy_j(4.0, 0.0, 10.0, 0.0, 4.0)
       + study_interval_overlap_energy_j(4.0, 0.0, 10.0, 4.0, 10.0),
       4.0, study_energy_conservation_tolerance_j(4.0), "adjacent windows conserve task energy");
}

void test_finite_makespan_and_static_energy() {
  StudyMeasurementObserver observer(finite_request(), {frame(0, 0.0, 5.0)});
  check(observer.process_releases_through(0.0), "finite work releases at time zero");
  check(observer.record_ready_event(0.0), "finite task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:0", "terminal", "RVV", 0.0, 5.0, energy(1e-6)), "finite task dispatch is recorded");
  complete_unit(observer, 0, 5.0);
  check(observer.finish_quiescent_timestamp(5.0, snapshot(5.0)), "finite terminal timestamp is observed");
  Config config;
  config.static_power_w = 2.0;
  const auto result = observer.finish(config, 1e6);
  check(result.execution_status == StudyExecutionStatus::Complete, "finite study completes after all work units");
  near(result.interval_duration_us, 5.0, 0.0, "finite makespan ends at final work-unit completion");
  near(*result.energy.dynamic_energy_j.value, 1e-6, 1e-18, "finite dynamic energy uses the simulator aggregate");
  near(*result.energy.static_energy_j.value, 10e-6, 1e-18, "static energy integrates modeled power over makespan");
  near(*result.energy.it_energy_j.value, 11e-6, 1e-18, "IT energy is dynamic plus modeled static energy");
  near(*result.energy.average_it_power_w.value, 2.2, 1e-12, "finite average IT power uses makespan energy");
  near(*result.completed_throughput_per_s.value, 200'000.0, 1e-8, "finite throughput uses completed units divided by makespan");
  near(*result.energy.energy_per_completed_work_unit_j.value, 11e-6, 1e-18, "finite energy per completed work unit uses all completed units");
}

void test_finite_multiunit_energy_denominator() {
  StudyMeasurementObserver observer(finite_request(), {frame(0, 0.0), frame(1, 0.0)});
  check(observer.process_releases_through(0.0), "both finite work units release");
  check(observer.record_ready_event(0.0), "first finite task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:0", "vector", "RVV", 0.0, 10.0, energy(2e-6)),
        "first finite task energy is attributed");
  check(observer.record_ready_event(0.0), "second finite task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:1", "matrix", "NPU", 0.0, 10.0, energy(0.0, 6e-6)),
        "second finite task energy is attributed");
  complete_unit(observer, 0, 10.0);
  complete_unit(observer, 1, 10.0);
  check(observer.finish_quiescent_timestamp(10.0, snapshot(10.0)), "finite run reaches its 10 us makespan");

  Config config;
  config.static_power_w = 0.2;
  const auto result = observer.finish(config, 8e6);
  check(result.execution_status == StudyExecutionStatus::Complete && result.completed_frames_by_drain == 2,
        "finite energy denominator includes both drained completed frames");
  near(*result.energy.dynamic_energy_j.value, 8e-6, 1e-18, "finite dynamic energy sums the two task amounts");
  near(*result.energy.static_energy_j.value, 2e-6, 1e-18, "finite static energy uses 0.2 W over 10 us");
  near(*result.energy.it_energy_j.value, 10e-6, 1e-18, "finite IT energy includes dynamic and static components");
  near(*result.energy.average_it_power_w.value, 1.0, 1e-12, "finite average IT power uses 10 us makespan");
  near(*result.energy.energy_per_completed_work_unit_j.value, 5e-6, 1e-18,
       "finite IT energy is divided by two completed work units, not one task or frame-stage");
}

void test_zero_duration_finite_power_is_unavailable() {
  StudyMeasurementObserver observer(finite_request(), {frame(0, 0.0)});
  check(observer.process_releases_through(0.0), "zero-duration finite work releases at time zero");
  complete_unit(observer, 0, 0.0);
  check(observer.finish_quiescent_timestamp(0.0, snapshot(0.0)), "zero-duration work completes at time zero");
  const auto result = observer.finish(Config{}, 0.0);
  check(result.execution_status == StudyExecutionStatus::Complete && result.interval_duration_us == 0.0,
        "a completed zero-makespan finite workload remains a complete measurement");
  check(!result.energy.average_it_power_w.value
        && result.energy.average_it_power_w.null_reason == "zero_duration_interval",
        "average power is unavailable rather than defined as zero over a zero-duration interval");
  check(result.energy.energy_per_completed_work_unit_j.value
        && *result.energy.energy_per_completed_work_unit_j.value == 0.0,
        "zero IT energy divided by one completed work unit remains a defined zero-energy result");
}

void test_static_energy_adjacent_windows_conserve() {
  Config config;
  config.static_power_w = 2.0;
  const auto first = model_accounting(config, 0.0, 1'000'000.0);
  const auto second = model_accounting(config, 0.0, 2'000'000.0);
  const auto combined = model_accounting(config, 0.0, 3'000'000.0);
  near(first.static_energy_pj / 1e12, 2.0, 1e-12, "one-second static energy is two joules");
  near(second.static_energy_pj / 1e12, 4.0, 1e-12, "two-second static energy is four joules");
  near((first.static_energy_pj + second.static_energy_pj) / 1e12, combined.static_energy_pj / 1e12,
       study_energy_conservation_tolerance_j(6.0), "adjacent static-energy intervals conserve energy");
}

void test_sustained_window_cohort_backlog_and_drain() {
  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Sustained;
  request.window_start_us = 5.0;
  request.window_end_us = 10.0;
  request.limits = limits(1000, 100.0, 20.0, 100);
  StudyMeasurementObserver observer(request, {frame(0, 0.0), frame(1, 4.0), frame(2, 5.0, 7.0),
                                                frame(3, 9.0, 0.0), frame(4, 10.0)});
  check(observer.process_releases_through(0.0), "warm-up frame is released");
  check(observer.finish_quiescent_timestamp(0.0, snapshot(0.0)), "warm-up timestamp is observed");

  check(observer.record_ready_event(2.0), "first frame task becomes ready");
  check(observer.record_task_dispatch(2.0, "frame:0", "crossing", "RVV", 2.0, 8.0, energy(12.0)), "first crossing task is recorded");
  check(observer.process_releases_through(4.0), "second warm-up frame release is observed");
  check(observer.record_ready_event(4.0), "second frame task becomes ready");
  check(observer.record_task_dispatch(4.0, "frame:1", "ends-at-boundary", "DMA+DRAM", 4.0, 10.0, energy(6.0)), "second crossing task is recorded");
  check(observer.process_releases_through(5.0), "release exactly at t0 enters the release cohort");
  check(observer.record_ready_event(5.0), "t0 work becomes ready");
  check(observer.record_task_dispatch(5.0, "frame:2", "drains-after-window", "NPU", 5.0, 12.0, energy(7.0)), "post-window draining task is recorded");
  check(observer.finish_quiescent_timestamp(5.0, snapshot(5.0)), "start boundary snapshot is captured after same-time releases");

  complete_unit(observer, 0, 8.0);
  check(observer.finish_quiescent_timestamp(8.0, snapshot(8.0)), "in-window completion timestamp is observed");
  check(observer.process_releases_through(9.0), "release inside the window enters the target cohort");
  check(observer.record_ready_event(9.0), "zero-duration work becomes ready");
  check(observer.record_task_dispatch(9.0, "frame:3", "zero-duration", "RVV", 9.0, 9.0, energy(3.0)), "zero-duration task is recorded");
  complete_unit(observer, 3, 9.0);
  check(observer.finish_quiescent_timestamp(9.0, snapshot(9.0)), "zero-duration completion is observed before t1");
  check(observer.process_releases_through(10.0), "t1 boundary is reached without admitting a release at t1");
  complete_unit(observer, 1, 10.0);
  check(observer.finish_quiescent_timestamp(10.0, snapshot(10.0)), "completion at t1 updates end backlog but not window completions");
  complete_unit(observer, 2, 12.0);
  check(observer.finish_quiescent_timestamp(12.0, snapshot(12.0)), "post-window drain completes the release cohort");

  Config config;
  config.static_power_w = 2.0;
  const auto result = observer.finish(config, 28e12);
  check(result.execution_status == StudyExecutionStatus::Complete, "sustained release cohort drains completely");
  check(result.scheduled_frames == 4, "release exactly at t1 is excluded from sustained schedule");
  check(result.frame_cohort.scheduled == 2 && result.frame_cohort.released == 2, "target cohort is releases in [t0,t1)");
  check(result.completed_frames_in_window == 2, "window throughput counts all frame completions strictly before t1");
  check(result.frame_cohort.completed_in_window == 1, "release-cohort completion-in-window count excludes the warm-up completion");
  check(result.frame_cohort.completed_by_drain == 2 && result.frame_cohort.incomplete == 0, "release cohort completion is measured after bounded drain");
  near(*result.frame_cohort.mean_latency_us.value, 3.5, 1e-12, "cohort latency includes a post-window completion");
  near(*result.frame_cohort.p99_latency_us.value, 6.93, 1e-12, "P99 uses linear interpolation at rank (N-1)*0.99");
  check(*result.frame_cohort.deadline_misses.value == 0, "latency equal to its deadline is on time");
  check(result.start_snapshot && result.start_snapshot->released_unfinished_work_units == 3, "B(t0) includes released incomplete units after t0 arrivals");
  check(result.end_snapshot && result.end_snapshot->released_unfinished_work_units == 1, "B(t1) excludes a unit completing exactly at t1");
  check(result.backlog_delta_work_units == -2, "backlog delta compares half-open boundary snapshots");
  check(result.operating_state == "no_growth_observed", "fixed-window non-growth is explicitly only an observation");
  near(*result.energy.dynamic_energy_j.value, 19.0, 1e-9, "window dynamic energy is frozen before post-window drain energy");
  near(*result.energy.static_energy_j.value, 10e-6, 1e-18, "sustained static energy uses exactly the observation duration");
  near(*result.completed_throughput_per_s.value, 400'000.0, 1e-8, "sustained throughput counts completions in [t0,t1)");
  near(*result.offered_throughput_per_s.value, 400'000.0, 1e-8, "offered throughput counts releases in [t0,t1)");
  near(*result.energy.energy_per_completed_work_unit_j.value, (19.0 + 10e-6) / 2.0, 1e-12,
       "sustained window IT energy uses only the two work units completed inside [t0,t1)");
}

void test_cohort_p99_deadline_equality_and_misses() {
  StudyMeasurementObserver observer(finite_request(), {frame(0, 0.0, 10.0), frame(1, 0.0, 19.0)});
  check(observer.process_releases_through(0.0), "both finite units are released");
  for (std::uint64_t i = 0; i < 2; ++i) {
    check(observer.record_ready_event(0.0), "finite task is ready");
    const double end = i == 0 ? 10.0 : 20.0;
    check(observer.record_task_dispatch(0.0, "frame:" + std::to_string(i), "latency", "RVV", 0.0, end, energy()), "finite task dispatch is recorded");
  }
  complete_unit(observer, 0, 10.0);
  complete_unit(observer, 1, 20.0);
  check(observer.finish_quiescent_timestamp(20.0, snapshot(20.0)), "finite cohort completes");
  const auto result = observer.finish(Config{}, 0.0);
  near(*result.frame_cohort.p99_latency_us.value, 19.9, 1e-12, "two-value P99 is linearly interpolated at 0.99");
  check(*result.frame_cohort.deadline_misses.value == 1, "deadline equality is on time while a strict overrun misses");
  near(*result.frame_cohort.deadline_miss_rate.value, 0.5, 1e-12, "deadline miss rate uses target cohort size");
}

void test_censoring_preserves_window_and_reports_incomplete_work() {
  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Sustained;
  request.window_start_us = 0.0;
  request.window_end_us = 2.0;
  request.limits = limits(100, 100.0, 2.0, 100);
  StudyMeasurementObserver observer(request, {frame(0, 0.0), frame(1, 1.0)});
  check(observer.process_releases_through(0.0), "censored cohort first release is recorded");
  check(observer.record_ready_event(0.0), "censored task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:0", "slow0", "RVV", 0.0, 3.0, energy(2.0)), "censored first task is recorded");
  check(observer.finish_quiescent_timestamp(0.0, snapshot(0.0)), "censored start boundary is observed");
  check(observer.process_releases_through(1.0), "censored cohort second release is recorded");
  check(observer.record_ready_event(1.0), "censored second task becomes ready");
  check(observer.record_task_dispatch(1.0, "frame:1", "slow1", "RVV", 1.0, 5.0, energy(2.0)), "censored second task is recorded");
  check(observer.finish_quiescent_timestamp(1.0, snapshot(1.0)), "pre-window-end backlog is observed");
  check(observer.finish_quiescent_timestamp(2.0, snapshot(2.0)), "measurement window freezes before drain");
  check(!observer.finish_quiescent_timestamp(4.0, snapshot(4.0)), "unfinished cohort is censored at drain limit");
  const auto result = observer.finish(Config{}, 4e12);
  check(result.execution_status == StudyExecutionStatus::Censored, "drain limit returns censored status");
  check(result.censor_reason == StudyCensorReason::MaxDrainSimulatedTime, "drain censor reason is deterministic");
  check(result.interval_complete && result.energy.dynamic_energy_j.value.has_value(), "completed window metrics survive a censored drain");
  check(result.final_unfinished_work_units == 2 && result.frame_cohort.incomplete == 2, "unfinished work is explicitly reported");
  check(!result.frame_cohort.p99_latency_us.value && result.frame_cohort.p99_latency_us.null_reason == "cohort_incomplete_after_censor", "latency statistics are censored rather than silently partial");
}

void test_finite_censor_preserves_partial_counts_and_null_metrics() {
  StudyMeasurementObserver observer(finite_request(limits(100, 150.0)), {frame(0, 0.0), frame(1, 100.0), frame(2, 200.0)});
  check(observer.process_releases_through(0.0), "first finite cohort member releases");
  check(observer.record_ready_event(0.0), "first finite task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:0", "fast", "RVV", 0.0, 50.0, energy()), "first finite task dispatches");
  complete_unit(observer, 0, 50.0);
  check(observer.finish_quiescent_timestamp(50.0, snapshot(50.0)), "first unit completion is observed");
  check(observer.process_releases_through(100.0), "second finite cohort member releases");
  check(observer.record_ready_event(100.0), "second finite task becomes ready");
  check(observer.record_task_dispatch(100.0, "frame:1", "slow", "RVV", 100.0, 200.0, energy()), "second finite task dispatches before cap");
  check(observer.finish_quiescent_timestamp(100.0, snapshot(100.0)), "second unit remains in flight");
  check(!observer.finish_quiescent_timestamp(150.0, snapshot(150.0)), "finite run censors at absolute time limit");
  const auto result = observer.finish(Config{}, 0.0);
  check(result.execution_status == StudyExecutionStatus::Censored && result.censor_reason == StudyCensorReason::MaxSimulatedTime,
        "finite time cap has explicit censored status");
  near(result.interval_start_us, 0.0, 0.0, "finite censored interval starts at zero");
  near(result.interval_end_us, 150.0, 0.0, "finite censored interval ends at model-time cap");
  near(result.interval_duration_us, 150.0, 0.0, "finite censored duration is retained for provenance");
  check(!result.interval_complete && result.scheduled_frames == 3 && result.released_frames == 2,
        "finite censor preserves scheduled and released counts");
  check(result.completed_frames_by_drain == 1 && result.frame_cohort.incomplete == 2,
        "finite censor preserves completion and incomplete counts");
  check(result.final_unfinished_work_units == 1 && result.final_scheduled_not_released_work_units == 1,
        "finite censor separates in-flight from not-yet-released work");
  check(!result.completed_throughput_per_s.value && !result.frame_cohort.p99_latency_us.value
        && !result.energy.dynamic_energy_j.value && !result.energy.static_energy_j.value,
        "finite partial interval metrics are null instead of undercounted");
}

void test_frame_and_control_cohorts_are_separate() {
  StudyWorkUnitSpec control{"control:0", StudyWorkUnitKind::Control, 0, 0.0, 3.0};
  StudyMeasurementObserver observer(finite_request(), {frame(0, 0.0, 10.0), control});
  check(observer.process_releases_through(0.0), "frame and control work release separately");
  check(observer.record_ready_event(0.0), "frame task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:0", "frame", "RVV", 0.0, 2.0, energy(2e-6)), "frame task dispatch is recorded");
  check(observer.record_ready_event(0.0), "control task becomes ready");
  check(observer.record_task_dispatch(0.0, "control:0", "control", "RVV", 0.0, 4.0, energy(4e-6)), "control task dispatch is recorded");
  complete_unit(observer, 0, 2.0);
  check(observer.record_task_completion_event(4.0), "control completion event is recorded");
  observer.record_work_unit_completion("control:0", 4.0);
  check(observer.finish_quiescent_timestamp(4.0, snapshot(4.0)), "separate cohorts complete");
  // Finite accounting receives the simulator's whole-run energy sum. The
  // synthetic dispatches above independently total 6 µJ (6e6 pJ).
  const auto result = observer.finish(Config{}, 6e6);
  check(result.frame_cohort.scheduled == 1 && result.control_cohort.scheduled == 1, "frame and control scheduled counts are distinct");
  check(result.frame_cohort.deadline_misses.value && *result.frame_cohort.deadline_misses.value == 0, "frame deadline outcome is separate");
  check(result.control_cohort.deadline_misses.value && *result.control_cohort.deadline_misses.value == 1, "control deadline outcome is separate");
  near(*result.energy.dynamic_energy_j.value, 6e-6, 1e-18, "finite dynamic energy includes frame and control work");
  near(*result.energy.energy_per_completed_work_unit_j.value, 3e-6, 1e-18,
       "finite per-unit denominator counts completed frames and completed control work");
}

void test_empty_measurement_inputs_are_invalid() {
  StudyMeasurementObserver finite(finite_request(), {});
  check(finite.is_invalid(), "empty finite schedule is invalid");
  check(finite.finish(Config{}, 0.0).error_code == "empty_workload_unsupported", "empty finite input has stable error code");

  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Sustained;
  request.window_start_us = 5.0;
  request.window_end_us = 10.0;
  request.limits = limits(100, 100.0, 10.0, 100);
  StudyMeasurementObserver warmup_only(request, {frame(0, 0.0), frame(1, 4.0)});
  check(warmup_only.is_invalid(), "sustained schedule with no in-window cohort is invalid");
  check(warmup_only.finish(Config{}, 0.0).error_code == "empty_target_cohort_unsupported", "empty sustained target has stable error code");
}

void test_task_event_cap_stops_before_next_event() {
  StudyMeasurementObserver observer(finite_request(limits(1)), {frame(0, 0.0)});
  check(observer.process_releases_through(0.0), "first release event consumes the sole event budget");
  check(!observer.record_ready_event(0.0), "event beyond limit is not processed");
  check(observer.is_censored(), "event cap censors the candidate");
  const auto result = observer.finish(Config{}, 0.0);
  check(result.task_event_count == 1 && result.censor_reason == StudyCensorReason::MaxTaskEvents, "event cap reports exact count and reason");
}

void test_model_time_and_backlog_limits() {
  StudyMeasurementObserver time_limited(finite_request(limits(100, 5.0)), {frame(0, 0.0)});
  check(time_limited.process_releases_through(0.0), "time-limited work unit is released");
  check(time_limited.record_ready_event(0.0), "time-limited task becomes ready");
  check(time_limited.record_task_dispatch(0.0, "frame:0", "beyond-limit", "RVV", 0.0, 10.0, energy()), "long task is dispatched before model-time limit");
  check(!time_limited.finish_quiescent_timestamp(5.0, snapshot(5.0)), "unfinished work censors at absolute model-time limit");
  check(time_limited.is_censored(), "model-time limit has a censored status");
  check(time_limited.finish(Config{}, 0.0).censor_reason == StudyCensorReason::MaxSimulatedTime, "model-time limit reason is explicit");

  StudyMeasurementObserver backlog_limited(finite_request(limits(100, 100.0, 10.0, 1)), {frame(0, 0.0), frame(1, 0.0)});
  check(!backlog_limited.process_releases_through(0.0), "backlog strictly over threshold censors on the release event that exceeds the cap");
  check(backlog_limited.finish(Config{}, 0.0).censor_reason == StudyCensorReason::MaxBacklogWorkUnits, "backlog limit reason is explicit");
}

void test_events_exactly_on_time_and_drain_caps_are_processed() {
  StudyMeasurementObserver finite(finite_request(limits(100, 5.0)), {frame(0, 0.0)});
  check(finite.process_releases_through(0.0), "finite cap-boundary unit releases");
  check(finite.record_ready_event(0.0), "finite cap-boundary task becomes ready");
  check(finite.record_task_dispatch(0.0, "frame:0", "at-cap", "RVV", 0.0, 5.0, energy()), "finite cap-boundary task dispatches");
  complete_unit(finite, 0, 5.0);
  check(finite.finish_quiescent_timestamp(5.0, snapshot(5.0)), "completion at max simulated time is processed before censor decision");
  check(finite.finish(Config{}, 0.0).execution_status == StudyExecutionStatus::Complete,
        "work completing exactly at max simulated time is not censored");

  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Sustained;
  request.window_start_us = 0.0;
  request.window_end_us = 2.0;
  request.limits = limits(100, 100.0, 2.0, 10);
  StudyMeasurementObserver sustained(request, {frame(0, 0.0)});
  check(sustained.process_releases_through(0.0), "drain-cap boundary unit releases");
  check(sustained.record_ready_event(0.0), "drain-cap boundary task becomes ready");
  check(sustained.record_task_dispatch(0.0, "frame:0", "drain-at-cap", "RVV", 0.0, 4.0, energy()), "drain-cap task dispatches");
  check(sustained.finish_quiescent_timestamp(0.0, snapshot(0.0)), "sustained start boundary is captured");
  check(sustained.finish_quiescent_timestamp(2.0, snapshot(2.0)), "sustained window freezes before drain");
  complete_unit(sustained, 0, 4.0);
  check(sustained.finish_quiescent_timestamp(4.0, snapshot(4.0)), "completion at drain cap is processed before censor decision");
  check(sustained.finish(Config{}, 0.0).execution_status == StudyExecutionStatus::Complete,
        "work completing exactly at drain cap is not censored");
}

Config small_physical_config();

void compare_simulations(const Config& config, const std::string& label) {
  Simulator legacy(config);
  legacy.run();
  Simulator instrumented(config);
  StudyMeasurementRequest request = finite_request(limits(1'000'000, 10'000'000.0, 100'000.0, 100'000));
  const auto result = instrumented.run_study(request);
  check(result.execution_status == StudyExecutionStatus::Complete, label + " study execution completes");
  check(instrumented.trace().size() == legacy.trace().size(), label + " trace event count is unchanged");
  for (std::size_t i = 0; i < legacy.trace().size(); ++i) {
    const auto& a = legacy.trace()[i];
    const auto& b = instrumented.trace()[i];
    check(a.frame == b.frame && a.task == b.task && a.resource == b.resource && a.start_us == b.start_us && a.end_us == b.end_us,
          label + " scheduler trace is unchanged at event " + std::to_string(i));
  }
  check(instrumented.total_energy_pj() == legacy.total_energy_pj(), label + " aggregate energy is unchanged");
  check(instrumented.latencies_us() == legacy.latencies_us(), label + " latency samples are unchanged");
  check(result.task_intervals.size() == legacy.trace().size(), label + " observer records every dispatched task");
  near(*result.energy.dynamic_energy_j.value, legacy.total_energy_pj() / 1e12, 0.0, label + " observer dynamic energy matches legacy aggregate");
  double task_energy = 0.0;
  for (const auto& task : result.task_intervals) task_energy += task.total_dynamic_energy_j;
  near(task_energy, *result.energy.dynamic_energy_j.value,
       study_energy_conservation_tolerance_j(*result.energy.dynamic_energy_j.value),
       label + " task energy ledger reconciles with the aggregate");
  const double category_energy = *result.energy.vector_energy_j.value + *result.energy.npu_arithmetic_energy_j.value
      + *result.energy.dram_energy_j.value + *result.energy.dma_energy_j.value;
  near(category_energy, *result.energy.dynamic_energy_j.value,
       study_energy_conservation_tolerance_j(*result.energy.dynamic_energy_j.value),
       label + " energy categories reconcile with the aggregate");
}

void check_sustained_scheduler(const Config& config, const std::string& label) {
  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Sustained;
  request.window_start_us = 0.0;
  request.window_end_us = 500.0;
  request.limits = limits(1'000'000, 10'000.0, 1'000.0, 100'000);
  Simulator simulator(config);
  const auto result = simulator.run_study(request);
  check(result.execution_status == StudyExecutionStatus::Complete, label + " sustained run completes");
  check(result.scheduled_frames == 1 && result.frame_cohort.scheduled == 1,
        label + " excludes releases at and after t1");
  check(result.released_frames == 1 && result.completed_frames_in_window == 1,
        label + " reports the admitted release and in-window completion");
  check(result.start_snapshot && result.end_snapshot, label + " records both window boundary snapshots");
  check(result.energy.dynamic_energy_j.value.has_value() && result.energy.average_it_power_w.value.has_value(),
        label + " reports window energy and average power");
}

void test_sustained_scheduler_cutoff_and_frozen_energy() {
  Config slow = small_physical_config();
  slow.frame_period_us = 100.0;
  slow.preprocess_elements = 1'000'000;
  validate(slow);
  auto run_with_drain = [&slow](double drain_us) {
    StudyMeasurementRequest request;
    request.mode = StudyMeasurementMode::Sustained;
    request.window_start_us = 100.0;
    request.window_end_us = 200.0;
    request.limits = limits(1'000'000, 10'000.0, drain_us, 100);
    Simulator simulator(slow);
    return simulator.run_study(request);
  };
  const auto short_drain = run_with_drain(50.0);
  const auto longer_drain = run_with_drain(100.0);
  for (const auto* result : {&short_drain, &longer_drain}) {
    check(result->execution_status == StudyExecutionStatus::Censored
          && result->censor_reason == StudyCensorReason::MaxDrainSimulatedTime,
          "long-running physical pipeline is bounded by drain time");
    check(result->scheduled_frames == 2 && result->released_frames == 2
          && result->final_scheduled_not_released_work_units == 0,
          "only releases strictly before t1 are scheduled");
    check(result->interval_complete && result->end_snapshot.has_value(), "window remains complete after a censored drain");
    check(result->final_unfinished_work_units > 0 && !result->frame_cohort.p99_latency_us.value,
          "unfinished target cohort is reported as censored");
    check(result->energy.dynamic_energy_j.value.has_value() && result->completed_throughput_per_s.value.has_value(),
          "completed observation-window metrics remain available");
    check(result->end_snapshot->in_flight_by_resource[1] == 1 && result->end_snapshot->ready_by_resource[1] == 1,
          "boundary snapshot distinguishes the RVV task in flight from the waiting task");
  }
  near(*short_drain.energy.dynamic_energy_j.value, *longer_drain.energy.dynamic_energy_j.value, 0.0,
       "additional drain time cannot change frozen window dynamic energy");
  near(*short_drain.completed_throughput_per_s.value, *longer_drain.completed_throughput_per_s.value, 0.0,
       "additional drain time cannot change the fixed-window throughput denominator or count");
  check(short_drain.end_snapshot->released_unfinished_work_units == longer_drain.end_snapshot->released_unfinished_work_units,
        "drain cannot mutate the t1 backlog snapshot");
  double reconstructed_window_energy = 0.0;
  for (const auto& task : short_drain.task_intervals) {
    reconstructed_window_energy += study_interval_overlap_energy_j(task.total_dynamic_energy_j,
        task.start_us, task.end_us, 100.0, 200.0);
  }
  near(reconstructed_window_energy, *short_drain.energy.dynamic_energy_j.value,
       study_energy_conservation_tolerance_j(reconstructed_window_energy),
       "runtime window energy reconciles with dispatched task intervals");
}

void test_scheduler_applies_event_and_model_time_caps() {
  const Config config = small_physical_config();
  StudyMeasurementRequest time_request = finite_request(limits(100'000, 0.001, 10.0, 100));
  Simulator time_limited(config);
  const auto time_result = time_limited.run_study(time_request);
  check(time_result.execution_status == StudyExecutionStatus::Censored
        && time_result.censor_reason == StudyCensorReason::MaxSimulatedTime,
        "production scheduler stops at its absolute model-time cap");
  near(time_result.last_model_time_us, 0.001, 0.0, "production scheduler reports exact model-time censor point");
  check(!time_result.energy.dynamic_energy_j.value && !time_result.completed_throughput_per_s.value,
        "finite scheduler censor does not report partial energy or throughput as complete");

  StudyMeasurementRequest event_request = finite_request(limits(1, 10'000.0, 10.0, 100));
  Simulator event_limited(config);
  const auto event_result = event_limited.run_study(event_request);
  check(event_result.execution_status == StudyExecutionStatus::Censored
        && event_result.censor_reason == StudyCensorReason::MaxTaskEvents && event_result.task_event_count == 1,
        "production scheduler stops before processing the event beyond its event cap");
}

void test_completed_backlog_growth_is_not_censored_or_called_stable() {
  StudyMeasurementRequest request;
  request.mode = StudyMeasurementMode::Sustained;
  request.window_start_us = 0.0;
  request.window_end_us = 2.0;
  request.limits = limits(100, 100.0, 10.0, 10);
  StudyMeasurementObserver observer(request, {frame(0, 0.0), frame(1, 1.0)});
  check(observer.process_releases_through(0.0), "first workload release is recorded");
  check(observer.record_ready_event(0.0), "first workload task becomes ready");
  check(observer.record_task_dispatch(0.0, "frame:0", "slow0", "RVV", 0.0, 3.0, energy()), "first workload task dispatches");
  check(observer.finish_quiescent_timestamp(0.0, snapshot(0.0)), "initial backlog snapshot is recorded");
  check(observer.process_releases_through(1.0), "second workload release is recorded");
  check(observer.record_ready_event(1.0), "second workload task becomes ready");
  check(observer.record_task_dispatch(1.0, "frame:1", "slow1", "RVV", 1.0, 4.0, energy()), "second workload task dispatches");
  check(observer.finish_quiescent_timestamp(1.0, snapshot(1.0)), "backlog grows before window end");
  check(observer.finish_quiescent_timestamp(2.0, snapshot(2.0)), "window records growing backlog at t1");
  complete_unit(observer, 0, 3.0);
  check(observer.finish_quiescent_timestamp(3.0, snapshot(3.0)), "first unit completes during drain");
  complete_unit(observer, 1, 4.0);
  check(observer.finish_quiescent_timestamp(4.0, snapshot(4.0)), "completed drain finishes second unit");
  const auto result = observer.finish(Config{}, 0.0);
  check(result.execution_status == StudyExecutionStatus::Complete, "fully drained backlog growth is completed, not censored");
  check(result.backlog_delta_work_units == 1 && result.operating_state == "backlog_growing",
        "positive fixed-window backlog delta is explicitly retained");
  check(result.target_cohort_complete && result.frame_cohort.p99_latency_us.value.has_value(),
        "fully drained overloaded cohort retains its complete latency metrics");
}

Config small_physical_config() {
  const std::string root = AXISSIM_SOURCE_DIR;
  return load_config(root + "/configs/baseline/manifest.conf", {
      {"frames", "3"}, {"frame_period_us", "1000"}, {"deadline_us", "1000"},
      {"input_bytes", "64"}, {"preprocess_elements", "64"}, {"preprocess_output_bytes", "128"},
      {"gemm_m", "4"}, {"gemm_n", "4"}, {"gemm_k", "4"},
      {"postprocess_elements", "8"}, {"postprocess_bytes", "8"}, {"dma_setup_us", "0.1"}});
}

void test_observer_enabled_and_disabled_equivalence_all_paths() {
  const Config physical = small_physical_config();
  compare_simulations(physical, "Physical AI event scheduler");
  check_sustained_scheduler(physical, "Physical AI event scheduler");

  Config with_control = physical;
  with_control.control_period_us = 50.0;
  with_control.control_deadline_us = 500.0;
  with_control.control_ops = 10;
  validate(with_control);
  compare_simulations(with_control, "Physical AI with control work");

  Config gemm_heavy = physical;
  gemm_heavy.scenario_name = "gemm-heavy-test";
  gemm_heavy.gemm_m = 64;
  gemm_heavy.gemm_n = 64;
  gemm_heavy.gemm_k = 128;
  gemm_heavy.npu_tile_m = 32;
  gemm_heavy.npu_tile_n = 32;
  gemm_heavy.npu_tile_k = 64;
  validate(gemm_heavy);
  compare_simulations(gemm_heavy, "GEMM-heavy event scheduler");
  check_sustained_scheduler(gemm_heavy, "GEMM-heavy event scheduler");

  Config vector = physical;
  vector.workload_mode = "rvv_stream";
  vector.preprocess_elements = 257;
  validate(vector);
  compare_simulations(vector, "RVV stream event scheduler");
  check_sustained_scheduler(vector, "RVV stream event scheduler");

  const std::string root = AXISSIM_SOURCE_DIR;
  const Config mnist = load_config(root + "/configs/workloads/mnist12_profiled.conf", {
      {"frames", "2"}, {"frame_period_us", "1000"}, {"deadline_us", "1000"}});
  compare_simulations(mnist, "MNIST-12 profile scheduler");
  check_sustained_scheduler(mnist, "MNIST-12 profile scheduler");
}

}  // namespace

int main() {
  test_overlap_and_energy_partition_boundaries();
  test_finite_makespan_and_static_energy();
  test_finite_multiunit_energy_denominator();
  test_zero_duration_finite_power_is_unavailable();
  test_static_energy_adjacent_windows_conserve();
  test_sustained_window_cohort_backlog_and_drain();
  test_cohort_p99_deadline_equality_and_misses();
  test_censoring_preserves_window_and_reports_incomplete_work();
  test_finite_censor_preserves_partial_counts_and_null_metrics();
  test_frame_and_control_cohorts_are_separate();
  test_empty_measurement_inputs_are_invalid();
  test_task_event_cap_stops_before_next_event();
  test_model_time_and_backlog_limits();
  test_events_exactly_on_time_and_drain_caps_are_processed();
  test_sustained_scheduler_cutoff_and_frozen_energy();
  test_scheduler_applies_event_and_model_time_caps();
  test_completed_backlog_growth_is_not_censored_or_called_stable();
  test_observer_enabled_and_disabled_equivalence_all_paths();
  std::cout << "All AxisSim study-measurement tests passed.\n";
  return EXIT_SUCCESS;
}
