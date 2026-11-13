#pragma once

#include "config.hpp"

namespace axissim {

// Scenario-derived accounting only. These values are not characterized silicon
// measurements; callers must retain the scenario and input assumptions.
struct ModelAccounting {
  double modeled_static_power_w = 0.0;
  double modeled_area_mm2 = 0.0;
  double static_energy_pj = 0.0;
  double it_energy_pj = 0.0;
  double facility_energy_pj = 0.0;
  double average_it_power_w = 0.0;
  double average_facility_power_w = 0.0;
  double deployment_facility_energy_kwh = 0.0;
  double deployment_electricity_cost = 0.0;
  double thermal_steady_state_c = 0.0;
  double thermal_end_c = 0.0;
  double reliability_expected_failures = 0.0;
  bool power_feasible = true;
  bool area_feasible = true;
  bool thermal_feasible = true;
};

[[nodiscard]] double modeled_static_power_w(const Config& config);
[[nodiscard]] double modeled_area_mm2(const Config& config);
// dynamic_energy_pj is aggregate activity energy from the simulator; makespan_us
// is the completed run duration. This applies the existing configured static
// power model and derives shared aggregate energy/power/budget outputs. The
// current model has no separate idle/leakage term: configured static power is
// charged throughout the full makespan.
[[nodiscard]] ModelAccounting model_accounting(const Config& config, double dynamic_energy_pj, double makespan_us);

}  // namespace axissim
