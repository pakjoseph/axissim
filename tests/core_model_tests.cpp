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

}  // namespace

int main() {
  try {
    test_rvv_stripmining_contract();
    test_npu_tiling_contract();
    test_iso_performance_energy_contract();
    test_scheduler_trace_contract();
    test_accounting_proxy_contract();
    std::cout << "AxisSim core model API contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "AxisSim core model API contract failed: " << error.what() << '\n';
    return 1;
  }
}
