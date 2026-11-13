#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "config.hpp"
#include "metrics.hpp"
#include "simulation.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_close(double actual, double expected, double tolerance, const std::string& message) {
  require(std::abs(actual - expected) <= tolerance,
          message + " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
}

void test_rvv_stripmining_contract() {
  axissim::Config config;
  config.workload_mode = "rvv_stream";
  config.preprocess_elements = 17;
  config.preprocess_ops_per_element = 1;
  axissim::validate(config);

  const axissim::Simulator simulator(config);
  require(simulator.vlmax() == 8, "256-bit VLEN with SEW=32 and LMUL=1 must expose VLMAX=8");
  require_close(simulator.tail_utilization(17), 17.0 / 24.0, 1e-12,
                "17 elements must occupy three eight-element vector groups");
  require(simulator.rvv_stream_compute_cycles() > 0.0, "a non-empty RVV stream must consume modeled cycles");
}

void test_npu_tiling_contract() {
  axissim::Config config;
  axissim::validate(config);
  const axissim::Simulator simulator(config);

  require(simulator.npu_tile_working_set_bytes() == 49'152,
          "default 64x64x128 tile must reconcile to its declared SRAM working set");
  require(simulator.unique_weight_bytes() == 262'144,
          "default GEMM weights must reconcile to K*N*element_bytes");
  require(simulator.npu_transfer_bytes(0) > simulator.unique_weight_bytes(),
          "first-frame NPU transfer must include activations, outputs, and tiled weights");
}

void test_iso_performance_energy_contract() {
  axissim::Config baseline_config;
  baseline_config.frames = 1;
  axissim::validate(baseline_config);
  axissim::Simulator baseline(baseline_config);
  baseline.run();

  axissim::Config reduced_compute_energy = baseline_config;
  reduced_compute_energy.npu_arithmetic_energy_scale = 1.0 / 3.0;
  axissim::validate(reduced_compute_energy);
  axissim::Simulator reduced(reduced_compute_energy);
  reduced.run();

  require_close(reduced.latencies_us().at(0), baseline.latencies_us().at(0), 1e-12,
                "an iso-performance arithmetic-energy scale must not alter modeled latency");
  require_close(reduced.npu_arithmetic_energy_pj(), baseline.npu_arithmetic_energy_pj() / 3.0, 1e-6,
                "arithmetic-energy scale must apply only to NPU arithmetic activity");
  require(reduced.total_energy_pj() < baseline.total_energy_pj(),
          "lower NPU arithmetic energy must lower total modeled dynamic energy");
}

void test_scheduler_trace_contract() {
  axissim::Config config;
  config.frames = 2;
  config.frame_period_us = 1'000.0;
  config.deadline_us = 1'000.0;
  axissim::validate(config);
  axissim::Simulator simulator(config);
  simulator.run();

  require(simulator.latencies_us().size() == 2, "two released frames must produce two completion latencies");
  require(!simulator.trace().empty(), "a completed run must expose a non-empty execution trace");
  require(simulator.resource_busy_us("RVV") > 0.0, "the physical-AI pipeline must schedule RVV work");
  require(simulator.makespan_us() >= simulator.latencies_us().back(),
          "run makespan must not precede the final observed frame latency");
}

void test_accounting_proxy_contract() {
  axissim::Config config;
  config.static_power_w = 1.0;
  config.pue = 1.5;
  config.deployment_xpu_count = 1'000;
  config.deployment_hours = 1.0;
  config.electricity_cost_per_kwh = 0.1;
  config.base_area_mm2 = 1.0;
  config.max_area_mm2 = 2.0;
  config.thermal_resistance_c_per_w = 10.0;
  config.thermal_capacitance_j_per_c = 1.0;
  config.max_junction_temp_c = 100.0;
  config.reliability_fit_rate = 100.0;
  axissim::validate(config);

  const axissim::ModelAccounting accounting = axissim::model_accounting(config, 0.0, 1'000'000.0);
  require_close(accounting.modeled_static_power_w, 1.0, 1e-12, "static power accounting must preserve explicit base power");
  require_close(accounting.average_facility_power_w, 1.5, 1e-12, "PUE must scale facility, not IT, power");
  require_close(accounting.deployment_facility_energy_kwh, 1.5, 1e-12, "deployment energy must be power times fleet and duration");
  require_close(accounting.deployment_electricity_cost, 0.15, 1e-12, "deployment cost must use explicit electricity price");
  require_close(accounting.reliability_expected_failures, 0.0001, 1e-12, "FIT extrapolation must remain scenario-derived");
  require(accounting.power_feasible && accounting.area_feasible && accounting.thermal_feasible,
          "an unconstrained one-watt scenario must remain feasible under its declared limits");
}

void test_accounting_energy_and_power_budget_boundaries() {
  axissim::Config config;
  config.static_power_w = 0.05;
  config.npu_macs = 1024;
  config.npu_static_power_per_mac_w = 0.00003;
  config.rvv_lanes = 8;
  config.rvv_static_power_per_lane_w = 0.002;
  config.sram_capacity_bytes = 131'072;
  config.sram_static_power_per_kib_w = 0.0001;
  config.dram_interface_static_power_w = 0.02;
  config.pue = 1.5;
  config.it_power_budget_w = 0.23;
  axissim::validate(config);

  const axissim::ModelAccounting accounting = axissim::model_accounting(config, 8'000'000.0, 2'000'000.0);
  require_close(accounting.modeled_static_power_w, 0.12952, 1e-12,
                "capacity and base static terms must retain the configured power equation");
  require_close(accounting.static_energy_pj, 259'040'000'000.0, 1e-3,
                "configured static power must be integrated over the full run makespan");
  require_close(accounting.it_energy_pj, 259'048'000'000.0, 1e-3,
                "IT energy must add simulator dynamic energy exactly once");
  require_close(accounting.facility_energy_pj, 388'572'000'000.0, 1e-3,
                "facility energy must apply PUE once to aggregate IT energy");
  require(accounting.power_feasible,
          "the 0.23 W reference must remain feasible under its existing modeled terms");

  axissim::Config boundary;
  boundary.static_power_w = 1.0;
  boundary.it_power_budget_w = 1.0;
  axissim::validate(boundary);
  const auto at_limit = axissim::model_accounting(boundary, 0.0, 1'000'000.0);
  require(at_limit.average_it_power_w == 1.0 && at_limit.power_feasible,
          "average IT power exactly equal to the budget must be feasible");

  boundary.it_power_budget_w = std::nextafter(1.0, 0.0);
  const auto above_limit = axissim::model_accounting(boundary, 0.0, 1'000'000.0);
  require(!above_limit.power_feasible,
          "a budget immediately below modeled average IT power must be infeasible");

  boundary.it_power_budget_w = 0.0;
  const auto no_budget = axissim::model_accounting(boundary, 0.0, 1'000'000.0);
  require(no_budget.power_feasible,
          "a zero power budget must continue to mean no configured constraint");
}

}  // namespace

int main() {
  try {
    test_rvv_stripmining_contract();
    test_npu_tiling_contract();
    test_iso_performance_energy_contract();
    test_scheduler_trace_contract();
    test_accounting_proxy_contract();
    test_accounting_energy_and_power_budget_boundaries();
    std::cout << "AxisSim core model API contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "AxisSim core model API contract failed: " << error.what() << '\n';
    return 1;
  }
}
