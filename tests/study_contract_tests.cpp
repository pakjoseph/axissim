#include <cmath>
#include <iostream>
#include <map>
#include <string>

#include "config.hpp"
#include "simulation.hpp"
#include "study_measurements.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* description) {
  if (!condition) { std::cerr << "FAIL: " << description << '\n'; ++failures; }
}

axissim::StudyMeasurementResult evaluate(double static_power_w) {
  auto config = axissim::load_config(std::string(AXISSIM_SOURCE_DIR) + "/configs/power_budget/manifest.conf", {
      {"frames","1"}, {"static_power_w",std::to_string(static_power_w)},
      {"npu_static_power_per_mac_w","0"}, {"rvv_static_power_per_lane_w","0"},
      {"sram_static_power_per_kib_w","0"}, {"dram_interface_static_power_w","0"},
      {"vector_op_pj","0"}, {"npu_mac_pj","0"}, {"dram_byte_pj","0"}, {"dma_byte_pj","0"}});
  axissim::StudyMeasurementRequest request;
  request.mode = axissim::StudyMeasurementMode::Finite;
  request.limits.max_task_events = 10000;
  request.limits.max_backlog_work_units = 100;
  request.limits.max_simulated_time_us = 1'000'000.0;
  request.limits.max_drain_simulated_us = 1'000'000.0;
  axissim::Simulator simulator(config);
  return simulator.run_study(request);
}
}  // namespace

int main() {
  const auto boundary = evaluate(0.23);
  check(boundary.execution_status == axissim::StudyExecutionStatus::Complete, "finite candidate completes using the M2 execution path");
  check(boundary.power_budget_feasible.has_value(), "candidate returns centralized model-accounting budget status");
  check(boundary.power_budget_feasible.value_or(false), "exact 0.23 W candidate passes the inclusive budget boundary");
  check(boundary.energy.average_it_power_w.value.has_value() && std::abs(*boundary.energy.average_it_power_w.value - 0.23) < 1e-12,
        "candidate average IT power uses the centralized accounting result");
  check(!boundary.task_intervals.empty(), "study observer retains dispatched tasks for result and trace export");
  for (const auto& task : boundary.task_intervals) {
    check(task.completion_observed && task.completion_status == "completed", "completed dispatch has an observed completion record");
    check(task.release_us <= task.ready_us && task.ready_us <= task.start_us && task.start_us <= task.end_us,
          "task trace timestamps preserve release, ready, dispatch, and completion order");
  }

  const auto over = evaluate(0.2301);
  check(over.execution_status == axissim::StudyExecutionStatus::Complete, "over-budget candidate remains a completed simulation");
  check(over.power_budget_feasible.has_value() && !*over.power_budget_feasible, "above-budget candidate fails without changing execution status");

  const double whole = axissim::study_interval_overlap_energy_j(7.0, 2.0, 12.0, 0.0, 20.0);
  const double left = axissim::study_interval_overlap_energy_j(7.0, 2.0, 12.0, 0.0, 7.0);
  const double right = axissim::study_interval_overlap_energy_j(7.0, 2.0, 12.0, 7.0, 20.0);
  check(std::abs((left + right) - whole) <= axissim::study_energy_conservation_tolerance_j(whole),
        "adjacent trace-window partitions conserve task energy");
  if (failures != 0) std::cerr << failures << " study contract core assertion(s) failed\n";
  return failures == 0 ? 0 : 1;
}
