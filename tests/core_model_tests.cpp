#include <algorithm>
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

void test_accounting_matches_independent_unit_derivation() {
  axissim::Config config;
  config.static_power_w = 0.125;
  config.npu_macs = 4;
  config.npu_static_power_per_mac_w = 0.03125;
  config.rvv_lanes = 4;
  config.rvv_static_power_per_lane_w = 0.03125;
  config.sram_capacity_bytes = 8'192;
  config.sram_static_power_per_kib_w = 0.015625;
  config.gemm_m = 2;
  config.gemm_n = 2;
  config.gemm_k = 2;
  config.npu_tile_m = 2;
  config.npu_tile_n = 2;
  config.npu_tile_k = 2;
  config.dram_interface_static_power_w = 0.125;
  config.pue = 1.5;
  config.it_power_budget_w = 0.875;
  axissim::validate(config);

  // Independent dimensional calculation: five 0.125 W terms make 0.625 W
  // static. 2.5e6 pJ over 10 us is 0.25 W dynamic, so IT power is 0.875 W.
  constexpr double expected_static_w = 0.125 + (4.0 * 0.03125) + (4.0 * 0.03125)
      + ((8'192.0 / 1'024.0) * 0.015625) + 0.125;
  constexpr double expected_dynamic_w = 2'500'000.0 / 10.0 / 1'000'000.0;
  constexpr double expected_static_pj = expected_static_w * 10.0 * 1'000'000.0;
  constexpr double expected_it_pj = 2'500'000.0 + expected_static_pj;
  constexpr double expected_it_w = expected_dynamic_w + expected_static_w;

  const auto accounting = axissim::model_accounting(config, 2'500'000.0, 10.0);
  require_close(accounting.modeled_static_power_w, expected_static_w, 0.0,
                "static power must match the independent sum of configured resource terms");
  require_close(accounting.static_energy_pj, expected_static_pj, 0.0,
                "static joules must convert from watts over microseconds to picojoules");
  require_close(accounting.it_energy_pj, expected_it_pj, 0.0,
                "IT energy must add dynamic and static energy once");
  require_close(accounting.average_it_power_w, expected_it_w, 0.0,
                "average IT watts must equal total energy divided by the 10 us interval");
  require_close(accounting.average_facility_power_w, expected_it_w * 1.5, 0.0,
                "PUE must scale facility power after IT power is calculated");
  require(accounting.power_feasible,
          "a candidate exactly at its independently derived modeled budget must pass");

  config.it_power_budget_w = std::nextafter(expected_it_w, 0.0);
  const auto below_budget = axissim::model_accounting(config, 2'500'000.0, 10.0);
  require(!below_budget.power_feasible,
          "a budget immediately below independently derived average IT power must fail");

  config.it_power_budget_w = 0.0;
  const auto disabled_budget = axissim::model_accounting(config, 0.0, 10.0);
  require(disabled_budget.power_feasible && disabled_budget.it_energy_pj == expected_static_pj,
          "zero dynamic activity leaves only interval static energy and zero budget disables the constraint");

  config.npu_macs = 8;
  config.sram_capacity_bytes = 16'384;
  const auto expanded_capacity = axissim::model_accounting(config, 0.0, 10.0);
  constexpr double expected_expanded_static_w = expected_static_w
      + (4.0 * 0.03125) + (8.0 * 0.015625);
  require_close(expanded_capacity.modeled_static_power_w, expected_expanded_static_w, 0.0,
                "doubling configured NPU and main-SRAM capacity adds only their independently priced static terms");

  const auto zero_duration = axissim::model_accounting(config, 0.0, 0.0);
  require(zero_duration.static_energy_pj == 0.0 && zero_duration.average_it_power_w == 0.0,
          "the aggregate helper preserves its explicit zero-duration behavior");
}

void test_static_power_terms_are_independently_accounted() {
  const auto static_power = [](const axissim::Config& config) {
    return axissim::modeled_static_power_w(config);
  };

  axissim::Config base;
  base.static_power_w = 0.125;
  base.npu_macs = 4;
  base.npu_static_power_per_mac_w = 0.03125;
  base.rvv_lanes = 4;
  base.rvv_static_power_per_lane_w = 0.03125;
  base.sram_capacity_bytes = 8'192;
  base.sram_static_power_per_kib_w = 0.015625;
  base.dram_interface_static_power_w = 0.125;

  const auto check_term = [&](const char* label, axissim::Config config, double expected_w) {
    require_close(static_power(config), expected_w, 0.0, label);
  };
  axissim::Config config;
  config.static_power_w = 0.125;
  check_term("base static contribution is charged as configured watts", config, 0.125);
  config = {};
  config.npu_macs = 4;
  config.npu_static_power_per_mac_w = 0.03125;
  check_term("NPU capacity contribution is MAC count times W/MAC", config, 0.125);
  config = {};
  config.rvv_lanes = 4;
  config.rvv_static_power_per_lane_w = 0.03125;
  check_term("RVV capacity contribution is lane count times W/lane", config, 0.125);
  config = {};
  config.sram_capacity_bytes = 8'192;
  config.sram_static_power_per_kib_w = 0.015625;
  check_term("main SRAM contribution converts bytes to KiB before applying W/KiB", config, 0.125);
  config = {};
  config.dram_interface_static_power_w = 0.125;
  check_term("DRAM-interface static contribution is an additive configured watt value", config, 0.125);

  constexpr double expected_combined = 0.125 + (4.0 * 0.03125) + (4.0 * 0.03125)
      + (8.0 * 0.015625) + 0.125;
  require_close(static_power(base), expected_combined, 0.0,
                "independently verified static terms aggregate to the stated total");
}

void test_dynamic_energy_components_against_independent_activity_counts() {
  axissim::Config config;
  config.frames = 1;
  config.frame_period_us = 1'000.0;
  config.deadline_us = 1'000.0;
  config.input_bytes = 13;
  config.preprocess_elements = 5;
  config.preprocess_ops_per_element = 2;
  config.preprocess_output_bytes = 7;
  config.gemm_m = 2;
  config.gemm_n = 3;
  config.gemm_k = 4;
  config.postprocess_elements = 3;
  config.postprocess_ops_per_element = 4;
  config.postprocess_bytes = 2;
  config.vector_op_pj = 0.4;
  config.npu_mac_pj = 0.2;
  config.npu_arithmetic_energy_scale = 1.5;
  config.arithmetic_voltage_ratio = 0.5;
  config.dram_byte_pj = 3.0;
  config.dma_byte_pj = 0.5;

  for (const std::string policy : {"legacy_implicit", "deterministic_v1"}) {
    config.scheduler_ordering_policy = policy;
    axissim::validate(config);
    axissim::Simulator simulator(config);
    simulator.run();

    constexpr double vector_operations = (5.0 * 2.0) + (3.0 * 4.0);
    constexpr double expected_vector_pj = vector_operations * 0.4;
    constexpr double npu_macs = 2.0 * 3.0 * 4.0;
    constexpr double expected_npu_pj = npu_macs * 0.2 * 1.5 * 0.5 * 0.5;
    // Transfer bytes are independently counted: camera 13 + preprocess read
    // 13 + preprocess write 7 + one NPU tile (40 activation/output + 24 weight)
    // + postprocess read 2 = 99 bytes.
    constexpr double expected_transfer_bytes = 13.0 + 13.0 + 7.0 + 40.0 + 24.0 + 2.0;
    require(simulator.total_dram_bytes() == static_cast<std::uint64_t>(expected_transfer_bytes),
            "tiny physical pipeline must schedule the independently derived 99 transfer bytes");
    const double transfer_bytes = expected_transfer_bytes;
    const double expected_dram_pj = transfer_bytes * 3.0;
    const double expected_dma_pj = transfer_bytes * 0.5;
    const double expected_total_pj = expected_vector_pj + expected_npu_pj
        + transfer_bytes * (3.0 + 0.5);
    const double tolerance = std::max(1e-9, std::abs(expected_total_pj) * 1e-12);

    require_close(simulator.vector_energy_pj(), expected_vector_pj, 1e-12,
                  "vector energy must equal independently counted operations times pJ/op");
    require_close(simulator.npu_arithmetic_energy_pj(), expected_npu_pj, 1e-12,
                  "NPU energy must equal MAC count times scale and squared voltage ratio");
    require_close(simulator.dram_energy_pj(), expected_dram_pj, 1e-9,
                  "DRAM energy must equal transferred bytes times pJ/byte");
    require_close(simulator.dma_energy_pj(), expected_dma_pj, 1e-9,
                  "DMA energy must equal transferred bytes times pJ/byte");
    require_close(simulator.total_energy_pj(), expected_total_pj, tolerance,
                  "aggregate dynamic energy must equal the independently derived component sum");
  }
}

void test_dynamic_energy_sensitivities_are_isolated() {
  struct EnergySnapshot {
    double vector_pj;
    double npu_pj;
    double dram_pj;
    double dma_pj;
    double total_pj;
    double latency_us;
  };
  const auto run = [](axissim::Config config) {
    axissim::validate(config);
    axissim::Simulator simulator(config);
    simulator.run();
    return EnergySnapshot{simulator.vector_energy_pj(), simulator.npu_arithmetic_energy_pj(),
                          simulator.dram_energy_pj(), simulator.dma_energy_pj(),
                          simulator.total_energy_pj(), simulator.makespan_us()};
  };

  axissim::Config base;
  base.frames = 1;
  base.input_bytes = 13;
  base.preprocess_elements = 5;
  base.preprocess_ops_per_element = 2;
  base.preprocess_output_bytes = 7;
  base.gemm_m = 2;
  base.gemm_n = 3;
  base.gemm_k = 4;
  base.npu_tile_m = 2;
  base.npu_tile_n = 3;
  base.npu_tile_k = 4;
  base.postprocess_elements = 3;
  base.postprocess_ops_per_element = 4;
  base.postprocess_bytes = 2;
  base.vector_op_pj = 0.4;
  base.npu_mac_pj = 0.2;
  base.npu_arithmetic_energy_scale = 1.5;
  base.arithmetic_voltage_ratio = 0.5;
  base.dram_byte_pj = 3.0;
  base.dma_byte_pj = 0.5;
  base.dram_interface_static_power_w = 0.02;
  const auto reference = run(base);

  auto more_vector_ops = base;
  more_vector_ops.preprocess_ops_per_element *= 2;
  const auto vector = run(more_vector_ops);
  constexpr double added_vector_ops = 5.0 * 2.0;
  require_close(vector.vector_pj - reference.vector_pj, added_vector_ops * 0.4, 1e-12,
                "doubling preprocess operations adds only their independently counted vector energy");
  require_close(vector.npu_pj, reference.npu_pj, 1e-12, "vector activity must not alter NPU energy");
  require_close(vector.dram_pj, reference.dram_pj, 1e-12, "vector activity must not alter DRAM energy");
  require_close(vector.dma_pj, reference.dma_pj, 1e-12, "vector activity must not alter DMA energy");

  auto more_npu_energy = base;
  more_npu_energy.npu_arithmetic_energy_scale *= 2.0;
  const auto npu = run(more_npu_energy);
  require_close(npu.npu_pj, reference.npu_pj * 2.0, 1e-12,
                "doubling the NPU arithmetic scale doubles only NPU energy");
  require_close(npu.vector_pj, reference.vector_pj, 1e-12, "NPU scale must not alter vector energy");
  require_close(npu.dram_pj, reference.dram_pj, 1e-12, "NPU scale must not alter DRAM energy");
  require_close(npu.dma_pj, reference.dma_pj, 1e-12, "NPU scale must not alter DMA energy");
  require_close(npu.latency_us, reference.latency_us, 1e-12,
                "arithmetic-energy scaling must not change modeled execution timing");

  auto arithmetic_only = base;
  arithmetic_only.dram_byte_pj = 0.0;
  arithmetic_only.dma_byte_pj = 0.0;
  auto twice_the_macs = arithmetic_only;
  twice_the_macs.gemm_m *= 2;
  const auto arithmetic_reference = run(arithmetic_only);
  const auto doubled_macs = run(twice_the_macs);
  require_close(doubled_macs.npu_pj, arithmetic_reference.npu_pj * 2.0, 1e-12,
                "doubling GEMM MAC activity doubles NPU arithmetic energy with fixed coefficients");
  require_close(doubled_macs.vector_pj, arithmetic_reference.vector_pj, 1e-12,
                "doubling GEMM MAC activity must not alter vector energy");
  require_close(doubled_macs.dram_pj, arithmetic_reference.dram_pj, 1e-12,
                "zero configured DRAM coefficient isolates the arithmetic activity test");
  require_close(doubled_macs.dma_pj, arithmetic_reference.dma_pj, 1e-12,
                "zero configured DMA coefficient isolates the arithmetic activity test");

  auto more_voltage_sensitivity = base;
  more_voltage_sensitivity.arithmetic_voltage_ratio = 1.0;
  const auto voltage = run(more_voltage_sensitivity);
  require_close(voltage.npu_pj, reference.npu_pj * 4.0, 1e-12,
                "doubling voltage ratio changes only the NPU arithmetic term by its square");
  require_close(voltage.vector_pj, reference.vector_pj, 1e-12,
                "voltage-inspired arithmetic sensitivity must not alter vector energy");
  require_close(voltage.dram_pj, reference.dram_pj, 1e-12,
                "voltage-inspired arithmetic sensitivity must not alter DRAM energy");
  require_close(voltage.dma_pj, reference.dma_pj, 1e-12,
                "voltage-inspired arithmetic sensitivity must not alter DMA energy");
  require_close(voltage.latency_us, reference.latency_us, 1e-12,
                "voltage-inspired arithmetic-energy sensitivity must not alter latency");

  auto more_input_bytes = base;
  more_input_bytes.input_bytes *= 2;
  const auto traffic = run(more_input_bytes);
  constexpr double added_transfer_bytes = 2.0 * 13.0;  // Camera ingest plus preprocess read.
  require_close(traffic.dram_pj - reference.dram_pj, added_transfer_bytes * 3.0, 1e-9,
                "doubling input bytes adds DRAM energy for both input transfer stages");
  require_close(traffic.dma_pj - reference.dma_pj, added_transfer_bytes * 0.5, 1e-9,
                "doubling input bytes adds DMA energy for both input transfer stages");
  require_close(traffic.vector_pj, reference.vector_pj, 1e-12,
                "input transfer bytes must not alter vector operation energy");
  require_close(traffic.npu_pj, reference.npu_pj, 1e-12,
                "input transfer bytes must not alter NPU arithmetic energy");

  auto more_bandwidth = base;
  more_bandwidth.dram_bandwidth_gbps *= 2.0;
  auto more_resident_capacity = base;
  more_resident_capacity.weight_sram_capacity_bytes *= 2;
  more_resident_capacity.npu_weights_resident = 1;
  require_close(axissim::modeled_static_power_w(more_bandwidth), axissim::modeled_static_power_w(base), 0.0,
                "current static-power equation does not price DRAM bandwidth expansion");
  require_close(axissim::modeled_static_power_w(more_resident_capacity), axissim::modeled_static_power_w(base), 0.0,
                "current static-power equation does not price resident-weight SRAM capacity");
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
    test_accounting_matches_independent_unit_derivation();
    test_static_power_terms_are_independently_accounted();
    test_dynamic_energy_components_against_independent_activity_counts();
    test_dynamic_energy_sensitivities_are_isolated();
    std::cout << "AxisSim core model API contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "AxisSim core model API contract failed: " << error.what() << '\n';
    return 1;
  }
}
