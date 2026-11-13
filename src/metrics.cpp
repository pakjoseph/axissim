#include "metrics.hpp"

#include <cmath>

namespace axissim {

double modeled_static_power_w(const Config& config) {
  return config.static_power_w
       + static_cast<double>(config.npu_macs) * config.npu_static_power_per_mac_w
       + static_cast<double>(config.rvv_lanes) * config.rvv_static_power_per_lane_w
       + (static_cast<double>(config.sram_capacity_bytes) / 1024.0) * config.sram_static_power_per_kib_w
       + config.dram_interface_static_power_w;
}

double modeled_area_mm2(const Config& config) {
  return config.base_area_mm2
       + static_cast<double>(config.npu_macs) * config.npu_area_per_mac_mm2
       + static_cast<double>(config.rvv_lanes) * config.rvv_area_per_lane_mm2
       + (static_cast<double>(config.sram_capacity_bytes) / 1024.0) * config.sram_area_per_kib_mm2;
}

ModelAccounting model_accounting(const Config& config, double dynamic_energy_pj, double makespan_us) {
  ModelAccounting accounting;
  accounting.modeled_static_power_w = modeled_static_power_w(config);
  accounting.modeled_area_mm2 = modeled_area_mm2(config);
  accounting.static_energy_pj = accounting.modeled_static_power_w * makespan_us * 1'000'000.0;
  accounting.it_energy_pj = dynamic_energy_pj + accounting.static_energy_pj;
  accounting.facility_energy_pj = config.pue * accounting.it_energy_pj;
  accounting.average_it_power_w = makespan_us > 0.0 ? accounting.it_energy_pj / makespan_us / 1'000'000.0 : 0.0;
  accounting.average_facility_power_w = config.pue * accounting.average_it_power_w;
  accounting.deployment_facility_energy_kwh = accounting.average_facility_power_w * config.deployment_xpu_count * config.deployment_hours / 1000.0;
  accounting.deployment_electricity_cost = accounting.deployment_facility_energy_kwh * config.electricity_cost_per_kwh;
  accounting.thermal_steady_state_c = config.ambient_temp_c + accounting.average_it_power_w * config.thermal_resistance_c_per_w;
  const double tau_s = config.thermal_resistance_c_per_w * config.thermal_capacitance_j_per_c;
  accounting.thermal_end_c = tau_s > 0.0
      ? config.ambient_temp_c + (accounting.thermal_steady_state_c - config.ambient_temp_c) * (1.0 - std::exp(-(makespan_us / 1'000'000.0) / tau_s))
      : accounting.thermal_steady_state_c;
  accounting.reliability_expected_failures = config.reliability_fit_rate * config.reliability_voltage_fit_multiplier
      * config.deployment_xpu_count * config.deployment_hours / 1'000'000'000.0;
  accounting.power_feasible = config.it_power_budget_w == 0.0 || accounting.average_it_power_w <= config.it_power_budget_w;
  accounting.area_feasible = config.max_area_mm2 == 0.0 || accounting.modeled_area_mm2 <= config.max_area_mm2;
  accounting.thermal_feasible = config.max_junction_temp_c == 0.0 || accounting.thermal_end_c <= config.max_junction_temp_c;
  return accounting;
}

}  // namespace axissim
