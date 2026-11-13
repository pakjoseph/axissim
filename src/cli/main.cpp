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
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"
#include "metrics.hpp"
#include "profile.hpp"
#include "simulation.hpp"

namespace axissim {

[[nodiscard]] std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}




[[nodiscard]] double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  const double index = (values.size() - 1) * p;
  const auto low = static_cast<std::size_t>(std::floor(index));
  const auto high = static_cast<std::size_t>(std::ceil(index));
  return values[low] + (values[high] - values[low]) * (index - low);
}

[[nodiscard]] double max_output_interval_jitter_us(const std::vector<double>& completions, double nominal_period_us) {
  if (completions.size() < 2) return 0.0;
  double maximum = 0.0;
  for (std::size_t index = 1; index < completions.size(); ++index) {
    maximum = std::max(maximum, std::abs((completions[index] - completions[index - 1]) - nominal_period_us));
  }
  return maximum;
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

[[nodiscard]] std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    escaped.push_back(character);
  }
  return escaped;
}

void write_result_json(const std::string& path, const Simulator& simulator, const Config& config) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Unable to write JSON: " + path);
  const auto& latency = simulator.latencies_us();
  const double mean = std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size();
  const double energy_per_frame_uj = simulator.total_energy_pj() / latency.size() / 1'000'000.0;
  const ModelAccounting accounting = model_accounting(config, simulator.total_energy_pj(), simulator.makespan_us());
  const double static_power_w = accounting.modeled_static_power_w;
  const double static_energy_pj = accounting.static_energy_pj;
  const double it_energy_pj = accounting.it_energy_pj;
  const double average_it_power_w = accounting.average_it_power_w;
  const double deployment_facility_kwh = accounting.deployment_facility_energy_kwh;
  const double area_mm2 = accounting.modeled_area_mm2;
  const double steady_temp_c = accounting.thermal_steady_state_c;
  const double end_temp_c = accounting.thermal_end_c;
  const double expected_failures = accounting.reliability_expected_failures;
  const auto& queue_delay = simulator.vision_resource_queue_delays_us();
  const auto& control_response = simulator.control_response_times_us();
  const double queue_delay_mean = queue_delay.empty() ? 0.0 : std::accumulate(queue_delay.begin(), queue_delay.end(), 0.0) / queue_delay.size();
  const double queue_delay_max = queue_delay.empty() ? 0.0 : *std::max_element(queue_delay.begin(), queue_delay.end());
  const double control_response_max = control_response.empty() ? 0.0 : *std::max_element(control_response.begin(), control_response.end());
  const auto& profile_ledger = simulator.profile_transfer_ledger();
  const auto profile_ledger_bytes = std::accumulate(profile_ledger.cbegin(), profile_ledger.cend(), std::uint64_t{0},
                                                     [](std::uint64_t total, const ProfileTransferLedgerEntry& entry) { return total + entry.bytes; });
  output << std::fixed << std::setprecision(6);
  output << "{\n"
         << "  \"schema_version\": \"1.2\",\n"
         << "  \"scenario_name\": \"" << json_escape(config.scenario_name) << "\",\n"
         << "  \"model_version\": \"" << json_escape(config.model_version) << "\",\n"
         << "  \"workload_mode\": \"" << json_escape(config.workload_mode) << "\",\n"
         << "  \"mapping_plan\": \"" << json_escape(config.mapping_plan.empty() ? "implicit-profile-mapping" : std::filesystem::path(config.mapping_plan).filename().string()) << "\",\n"
         << "  \"scheduler_policy\": \"" << json_escape(config.scheduler_policy) << "\",\n"
         << "  \"arrival_mode\": \"" << json_escape(config.frame_arrival_mode) << "\",\n"
         << "  \"deadline_us\": " << config.deadline_us << ",\n"
         << "  \"assumptions\": {\"npu_arithmetic_energy_scale\": " << config.npu_arithmetic_energy_scale << ", \"arithmetic_voltage_ratio\": " << config.arithmetic_voltage_ratio << ", \"base_static_power_w\": " << config.static_power_w << ", \"npu_static_power_per_mac_w\": " << config.npu_static_power_per_mac_w << ", \"rvv_static_power_per_lane_w\": " << config.rvv_static_power_per_lane_w << ", \"sram_static_power_per_kib_w\": " << config.sram_static_power_per_kib_w << ", \"dram_interface_static_power_w\": " << config.dram_interface_static_power_w << ", \"pue\": " << config.pue << ", \"it_power_budget_w\": " << config.it_power_budget_w << "},\n"
         << "  \"timing\": {\"makespan_us\": " << simulator.makespan_us() << "},\n"
         << "  \"latency\": {\"mean_us\": " << mean << ", \"p99_us\": " << percentile(latency, 0.99) << "},\n"
         << "  \"deadlines\": {\"vision_misses\": " << simulator.deadline_misses() << ", \"vision_jobs\": " << latency.size()
         << ", \"control_misses\": " << simulator.control_deadline_misses() << ", \"control_jobs\": " << simulator.control_tasks() << "},\n"
         << "  \"qos\": {\"vision_queue_delay_mean_us\": " << queue_delay_mean << ", \"vision_queue_delay_max_us\": " << queue_delay_max
         << ", \"output_interval_jitter_max_us\": " << max_output_interval_jitter_us(simulator.vision_completion_times_us(), config.frame_period_us)
         << ", \"control_response_max_us\": " << control_response_max << ", \"max_pending_tasks\": " << simulator.max_pending_tasks() << "},\n"
         << "  \"traffic\": {\"dram_bytes\": " << simulator.total_dram_bytes() << "},\n"
         << "  \"profile_transfer_ledger\": {\"per_frame_bytes\": " << profile_ledger_bytes
         << ", \"first_frame_bytes\": " << simulator.profile_first_frame_transfer_bytes()
         << ", \"steady_state_bytes\": " << simulator.profile_steady_state_transfer_bytes() << ", \"entries\": [";
  for (std::size_t index = 0; index < profile_ledger.size(); ++index) {
    const auto& entry = profile_ledger[index];
    if (index > 0) output << ", ";
    output << "{\"operator_id\": \"" << json_escape(entry.operator_id) << "\", \"category\": \"" << json_escape(entry.category) << "\", \"bytes\": " << entry.bytes << "}";
  }
  output << "]},\n"
         << "  \"mapping_effects\": {\"fusion_count\": " << simulator.profile_fusion_count()
         << ", \"fused_intermediate_bytes_avoided_per_frame\": " << simulator.profile_fused_intermediate_bytes_avoided()
         << ", \"persistent_weight_operator_count\": " << simulator.profile_persistent_weight_operator_count()
         << ", \"persistent_weight_bytes_avoided_per_steady_frame\": " << simulator.profile_persistent_weight_bytes_avoided_per_steady_frame()
         << ", \"source_storage_precision\": \"" << json_escape(simulator.profile_source_storage_precision()) << "\""
         << ", \"target_storage_precision\": \"" << json_escape(simulator.profile_target_storage_precision()) << "\""
         << ", \"precision_traffic_bytes_avoided_per_frame\": " << simulator.profile_precision_traffic_bytes_avoided_per_frame()
         << ", \"mapped_npu_arithmetic_energy_scale\": " << simulator.profile_mapped_npu_arithmetic_energy_scale() << "},\n"
         << "  \"accuracy_evidence\": {\"status\": \"" << json_escape(simulator.profile().accuracy_evidence_status) << "\""
         << ", \"metric\": \"" << json_escape(simulator.profile().accuracy_metric) << "\""
         << ", \"value\": " << simulator.profile().accuracy_value
         << ", \"dataset\": \"" << json_escape(simulator.profile().accuracy_dataset) << "\""
         << ", \"evaluator\": \"" << json_escape(simulator.profile().accuracy_evaluator) << "\""
         << ", \"source_url\": \"" << json_escape(simulator.profile().accuracy_source_url) << "\"},\n"
         << "  \"energy\": {\"per_frame_uj\": " << energy_per_frame_uj << ", \"static_per_frame_uj\": " << static_energy_pj / latency.size() / 1'000'000.0 << ", \"it_per_frame_uj\": " << it_energy_pj / latency.size() / 1'000'000.0 << ", \"facility_per_frame_uj\": " << accounting.facility_energy_pj / latency.size() / 1'000'000.0 << ", \"vector_pj\": " << simulator.vector_energy_pj()
         << ", \"npu_arithmetic_pj\": " << simulator.npu_arithmetic_energy_pj() << ", \"dram_pj\": " << simulator.dram_energy_pj()
         << ", \"dma_pj\": " << simulator.dma_energy_pj() << "},\n"
         << "  \"power\": {\"base_static_w\": " << config.static_power_w << ", \"modeled_static_w\": " << static_power_w << ", \"average_it_w\": " << average_it_power_w << ", \"average_facility_w\": " << accounting.average_facility_power_w << ", \"budget_w\": " << config.it_power_budget_w << ", \"pue\": " << config.pue << ", \"feasible\": " << (accounting.power_feasible ? "true" : "false") << "},\n"
         << "  \"deployment\": {\"xpu_count\": " << config.deployment_xpu_count << ", \"hours\": " << config.deployment_hours << ", \"electricity_cost_per_kwh\": " << config.electricity_cost_per_kwh << ", \"facility_energy_kwh\": " << deployment_facility_kwh << ", \"electricity_cost\": " << accounting.deployment_electricity_cost << "},\n"
         << "  \"area_proxy\": {\"base_mm2\": " << config.base_area_mm2 << ", \"npu_per_mac_mm2\": " << config.npu_area_per_mac_mm2 << ", \"rvv_per_lane_mm2\": " << config.rvv_area_per_lane_mm2 << ", \"sram_per_kib_mm2\": " << config.sram_area_per_kib_mm2 << ", \"modeled_mm2\": " << area_mm2 << ", \"limit_mm2\": " << config.max_area_mm2 << ", \"feasible\": " << (accounting.area_feasible ? "true" : "false") << "},\n"
         << "  \"thermal_proxy\": {\"ambient_c\": " << config.ambient_temp_c << ", \"resistance_c_per_w\": " << config.thermal_resistance_c_per_w << ", \"capacitance_j_per_c\": " << config.thermal_capacitance_j_per_c << ", \"end_c\": " << end_temp_c << ", \"steady_state_c\": " << steady_temp_c << ", \"limit_c\": " << config.max_junction_temp_c << ", \"feasible\": " << (accounting.thermal_feasible ? "true" : "false") << "},\n"
         << "  \"reliability_proxy\": {\"fit_rate\": " << config.reliability_fit_rate << ", \"voltage_multiplier\": " << config.reliability_voltage_fit_multiplier << ", \"expected_failures\": " << expected_failures << "}\n"
         << "}\n";
}

void print_results(const Simulator& simulator, const Config& config) {
  const auto& latency = simulator.latencies_us();
  const double mean = std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size();
  const double energy_per_frame_uj = simulator.total_energy_pj() / latency.size() / 1'000'000.0;
  const ModelAccounting accounting = model_accounting(config, simulator.total_energy_pj(), simulator.makespan_us());
  const double static_power_w = accounting.modeled_static_power_w;
  const double static_energy_pj = accounting.static_energy_pj;
  const double it_energy_pj = accounting.it_energy_pj;
  const double average_it_power_w = accounting.average_it_power_w;
  const double deployment_facility_kwh = accounting.deployment_facility_energy_kwh;
  const double area_mm2 = accounting.modeled_area_mm2;
  const double steady_temp_c = accounting.thermal_steady_state_c;
  const double end_temp_c = accounting.thermal_end_c;
  const double expected_failures = accounting.reliability_expected_failures;
  const auto& queue_delay = simulator.vision_resource_queue_delays_us();
  const auto& control_response = simulator.control_response_times_us();
  const double queue_delay_mean = queue_delay.empty() ? 0.0 : std::accumulate(queue_delay.begin(), queue_delay.end(), 0.0) / queue_delay.size();
  const double queue_delay_max = queue_delay.empty() ? 0.0 : *std::max_element(queue_delay.begin(), queue_delay.end());
  const double control_response_max = control_response.empty() ? 0.0 : *std::max_element(control_response.begin(), control_response.end());
  const auto per_frame_uj = [&latency](double energy_pj) { return energy_pj / latency.size() / 1'000'000.0; };
  const double makespan = simulator.makespan_us();
  const double vector_ops_per_frame = config.workload_mode == "profiled_graph" ? simulator.profile_vector_ops_per_frame() : static_cast<double>(config.preprocess_elements) * config.preprocess_ops_per_element + (config.workload_mode == "physical_ai" ? static_cast<double>(config.postprocess_elements) * config.postprocess_ops_per_element : 0.0);
  const double npu_macs_per_frame = config.workload_mode == "profiled_graph" ? simulator.profile_npu_macs_per_frame() : config.workload_mode == "physical_ai" ? static_cast<double>(config.gemm_m) * config.gemm_n * config.gemm_k : 0.0;
  const auto utilization = [&simulator, makespan](const std::string& resource) {
    return makespan > 0.0 ? 100.0 * simulator.resource_busy_us(resource) / makespan : 0.0;
  };
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "AxisSim results\n"
            << "  scenario / model:    " << config.scenario_name << " / " << config.model_version << "\n"
            << "  workload mode:        " << config.workload_mode << "\n"
            << "  frame arrivals:       " << config.frame_arrival_mode << "\n"
            << "  RVV timing model:     " << config.rvv_timing_model << "\n"
            << "  RVV memory accounting:" << config.rvv_memory_accounting << "\n"
            << "  arithmetic voltage ratio: " << config.arithmetic_voltage_ratio << " (energy sensitivity only)\n"
            << "  scheduler policy:    " << config.scheduler_policy << "\n"
            << "  frames:              " << latency.size() << "\n"
            << "  RVV VLMAX:           " << simulator.vlmax() << " elements\n"
            << "  workload vector ops / NPU MACs per frame: " << vector_ops_per_frame << " / " << npu_macs_per_frame << "\n"
            << "  RVV tail utilization: " << 100.0 * simulator.tail_utilization(config.preprocess_elements) << "%";
  if (config.workload_mode == "physical_ai") {
    std::cout << " / " << 100.0 * simulator.tail_utilization(config.postprocess_elements) << "%\n"
              << "  NPU tile / SRAM:     " << simulator.npu_tile_working_set_bytes() << " / " << config.sram_capacity_bytes << " bytes\n"
              << "  NPU DRAM first/steady frame: " << static_cast<double>(simulator.npu_transfer_bytes(0)) / 1'000'000.0 << " / "
              << static_cast<double>(simulator.npu_steady_state_dram_bytes_per_frame()) / 1'000'000.0 << " MB\n"
              << "  persistent weights / double buffer: " << (config.npu_weights_resident == 1 ? "on" : "off") << " / "
              << (config.dma_double_buffering == 1 ? "on" : "off") << "\n";
  } else if (config.workload_mode == "rvv_stream") {
    std::cout << "\n  stream scope:         RVV compute only (no DMA, cache, scalar, or NPU stages)\n";
  } else {
    std::cout << "\n  mapping plan:         " << (config.mapping_plan.empty() ? "implicit-profile-mapping" : std::filesystem::path(config.mapping_plan).filename().string())
              << "\n  profile scope:        Static operation/traffic characterization; mapping-policy assumptions, not measured runtime or compiler validation\n";
    if (simulator.profile_fusion_count() > 0) {
      std::cout << "  fused intermediate traffic avoided: " << simulator.profile_fused_intermediate_bytes_avoided() << " bytes / frame\n";
    }
    if (simulator.profile_persistent_weight_operator_count() > 0) {
      std::cout << "  persistent NPU weight operators: " << simulator.profile_persistent_weight_operator_count()
                << " (first / steady transfer: " << simulator.profile_first_frame_transfer_bytes() << " / "
                << simulator.profile_steady_state_transfer_bytes() << " bytes; "
                << simulator.profile_persistent_weight_bytes_avoided_per_steady_frame() << " bytes avoided after frame 0)\n";
    }
    if (simulator.profile_source_storage_precision() != simulator.profile_target_storage_precision()) {
      std::cout << "  profiled storage precision: " << simulator.profile_source_storage_precision() << " -> "
                << simulator.profile_target_storage_precision() << " (" << simulator.profile_precision_traffic_bytes_avoided_per_frame()
                << " bytes/frame traffic avoided; NPU arithmetic energy scale " << simulator.profile_mapped_npu_arithmetic_energy_scale()
                << ", mapping assumption only)\n";
    }
  }
  std::cout << "  latency mean / P99:  " << mean << " / " << percentile(latency, 0.99) << " us\n"
            << "  latency min / max:   " << *std::min_element(latency.begin(), latency.end()) << " / " << *std::max_element(latency.begin(), latency.end()) << " us\n"
            << "  vision queue delay mean / max: " << queue_delay_mean << " / " << queue_delay_max << " us\n"
            << "  output interval jitter max: " << max_output_interval_jitter_us(simulator.vision_completion_times_us(), config.frame_period_us) << " us\n"
            << "  max concurrent tasks:" << simulator.max_concurrent_tasks() << "\n"
            << "  max pending tasks:    " << simulator.max_pending_tasks() << "\n"
            << "  utilization DMA/RVV/NPU: " << utilization("DMA+DRAM") << "% / " << utilization("RVV") << "% / " << utilization("NPU") << "%\n"
            << "  deadline misses:     " << simulator.deadline_misses() << " / " << latency.size() << " (deadline " << config.deadline_us << " us)\n"
            << "  DRAM traffic:        " << static_cast<double>(simulator.total_dram_bytes()) / 1'000'000.0 << " MB\n"
            << "  energy per frame:    " << energy_per_frame_uj << " uJ (modeled estimate)\n"
            << "  energy vector/NPU/DRAM/DMA: " << per_frame_uj(simulator.vector_energy_pj()) << " / "
            << per_frame_uj(simulator.npu_arithmetic_energy_pj()) << " / " << per_frame_uj(simulator.dram_energy_pj())
            << " / " << per_frame_uj(simulator.dma_energy_pj()) << " uJ\n"
            << "  modeled static power: " << static_power_w << " W\n"
            << "  energy static/IT/facility: " << static_energy_pj / latency.size() / 1'000'000.0 << " / " << it_energy_pj / latency.size() / 1'000'000.0 << " / " << accounting.facility_energy_pj / latency.size() / 1'000'000.0 << " uJ\n"
            << "  average IT/facility power: " << average_it_power_w << " / " << accounting.average_facility_power_w << " W\n";
  if (config.it_power_budget_w > 0) std::cout << "  IT power budget:      " << config.it_power_budget_w << " W (" << (accounting.power_feasible ? "feasible" : "infeasible") << ")\n";
  if (config.deployment_hours > 0) std::cout << "  deployment facility energy / cost: " << deployment_facility_kwh << " kWh / " << accounting.deployment_electricity_cost << " (modeled extrapolation)\n";
  if (config.max_area_mm2 > 0 || area_mm2 > 0) std::cout << "  architecture area proxy: " << area_mm2 << " mm^2" << (config.max_area_mm2 > 0 ? (accounting.area_feasible ? " (feasible)" : " (infeasible)") : "") << "\n";
  if (config.thermal_resistance_c_per_w > 0) std::cout << "  first-order thermal proxy end / steady: " << end_temp_c << " / " << steady_temp_c << " C\n";
  if (config.reliability_fit_rate > 0 && config.deployment_hours > 0) std::cout << "  reliability proxy expected failures: " << expected_failures << " (assumption-driven FIT extrapolation)\n";
  if (simulator.control_tasks() > 0) {
    std::cout << "  control deadline misses: " << simulator.control_deadline_misses() << " / " << simulator.control_tasks()
              << " (deadline " << config.control_deadline_us << " us)\n"
              << "  control response max: " << control_response_max << " us\n";
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

struct SensitivityRange {
  std::string parameter;
  double low = 0.0;
  double nominal = 0.0;
  double high = 0.0;
  std::string unit;
  std::string evidence_class;
  std::string rationale;
};

struct AnalysisPoint {
  std::string analysis;
  std::string parameter;
  std::string setting;
  double mean_latency_us = 0.0;
  double p99_latency_us = 0.0;
  int deadline_misses = 0;
  double it_energy_per_frame_uj = 0.0;
  double average_it_power_w = 0.0;
  double thermal_end_c = 0.0;
  bool feasible = true;
};

[[nodiscard]] const std::set<std::string>& sensitivity_supported_keys() {
  static const std::set<std::string> keys = {
      "dram_bandwidth_gbps", "dma_setup_us", "rvv_frequency_ghz", "npu_frequency_ghz",
      "vector_op_pj", "npu_mac_pj", "npu_arithmetic_energy_scale", "arithmetic_voltage_ratio",
      "dram_byte_pj", "dma_byte_pj", "static_power_w", "npu_static_power_per_mac_w",
      "rvv_static_power_per_lane_w", "sram_static_power_per_kib_w", "dram_interface_static_power_w",
      "it_power_budget_w", "ambient_temp_c", "thermal_resistance_c_per_w", "thermal_capacitance_j_per_c",
      "max_junction_temp_c", "reliability_fit_rate", "reliability_voltage_fit_multiplier"};
  return keys;
}

[[nodiscard]] double strict_csv_double(const std::string& value, const std::string& field) {
  std::size_t consumed = 0;
  double result = 0.0;
  try {
    result = std::stod(value, &consumed);
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid sensitivity " + field + ": " + value);
  }
  if (consumed != value.size() || !std::isfinite(result)) throw std::runtime_error("Invalid sensitivity " + field + ": " + value);
  return result;
}

[[nodiscard]] std::vector<SensitivityRange> read_sensitivity_ranges(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Unable to open sensitivity ranges: " + path);
  std::string header;
  if (!std::getline(input, header) || trim(header) != "parameter,low,nominal,high,unit,evidence_class,rationale") {
    throw std::runtime_error("Sensitivity ranges header must be exactly: parameter,low,nominal,high,unit,evidence_class,rationale");
  }
  std::vector<SensitivityRange> ranges;
  std::set<std::string> seen;
  std::string line;
  while (std::getline(input, line)) {
    if (trim(line).empty() || trim(line).starts_with('#')) continue;
    const auto fields = split_csv_values(line);
    if (fields.size() != 7) throw std::runtime_error("Sensitivity range rows must have seven comma-free fields");
    SensitivityRange range;
    range.parameter = fields[0];
    range.low = strict_csv_double(fields[1], "low bound");
    range.nominal = strict_csv_double(fields[2], "nominal value");
    range.high = strict_csv_double(fields[3], "high bound");
    range.unit = fields[4];
    range.evidence_class = fields[5];
    range.rationale = fields[6];
    if (!sensitivity_supported_keys().contains(range.parameter)) throw std::runtime_error("Unsupported sensitivity parameter: " + range.parameter);
    if (range.low > range.nominal || range.nominal > range.high) throw std::runtime_error("Sensitivity bounds must satisfy low <= nominal <= high for " + range.parameter);
    if (range.unit.empty() || range.rationale.empty()) throw std::runtime_error("Sensitivity unit and rationale must be non-empty");
    if (range.evidence_class != "assumed_sensitivity" && range.evidence_class != "published_reference" && range.evidence_class != "reproduced_reference") {
      throw std::runtime_error("Sensitivity evidence_class must be assumed_sensitivity, published_reference, or reproduced_reference");
    }
    if (!seen.insert(range.parameter).second) throw std::runtime_error("Duplicate sensitivity parameter: " + range.parameter);
    ranges.push_back(std::move(range));
  }
  if (ranges.empty()) throw std::runtime_error("Sensitivity ranges must contain at least one parameter");
  if (ranges.size() > 4) throw std::runtime_error("Sensitivity analysis supports at most four parameters for bounded corner exploration");
  return ranges;
}

[[nodiscard]] std::string format_setting(double value) {
  std::ostringstream stream;
  stream << std::setprecision(12) << value;
  return stream.str();
}

[[nodiscard]] AnalysisPoint evaluate_analysis_point(const std::string& config_path, const std::map<std::string, std::string>& overrides,
                                                     std::string analysis, std::string parameter, std::string setting) {
  const Config config = load_config(config_path, overrides);
  validate(config);
  Simulator simulator(config);
  simulator.run();
  const auto& latency = simulator.latencies_us();
  const ModelAccounting accounting = model_accounting(config, simulator.total_energy_pj(), simulator.makespan_us());
  return {std::move(analysis), std::move(parameter), std::move(setting),
          std::accumulate(latency.begin(), latency.end(), 0.0) / latency.size(), percentile(latency, 0.99),
          simulator.deadline_misses(), accounting.it_energy_pj / latency.size() / 1'000'000.0,
          accounting.average_it_power_w, accounting.thermal_end_c,
          accounting.power_feasible && accounting.area_feasible && accounting.thermal_feasible};
}

void write_analysis_csv(const std::vector<AnalysisPoint>& points, std::ostream& output) {
  output << "analysis,parameter,setting,mean_latency_us,p99_latency_us,deadline_misses,it_energy_per_frame_uj,average_it_power_w,thermal_end_c,feasible\n";
  output << std::fixed << std::setprecision(6);
  for (const auto& point : points) {
    output << point.analysis << ',' << point.parameter << ',' << point.setting << ',' << point.mean_latency_us << ',' << point.p99_latency_us << ','
           << point.deadline_misses << ',' << point.it_energy_per_frame_uj << ',' << point.average_it_power_w << ',' << point.thermal_end_c << ','
           << (point.feasible ? "true" : "false") << '\n';
  }
}

void write_analysis_json(const std::vector<SensitivityRange>& ranges, const std::vector<AnalysisPoint>& points, std::ostream& output) {
  const auto [min_latency, max_latency] = std::minmax_element(points.cbegin(), points.cend(), [](const AnalysisPoint& left, const AnalysisPoint& right) {
    return left.mean_latency_us < right.mean_latency_us;
  });
  const auto [min_energy, max_energy] = std::minmax_element(points.cbegin(), points.cend(), [](const AnalysisPoint& left, const AnalysisPoint& right) {
    return left.it_energy_per_frame_uj < right.it_energy_per_frame_uj;
  });
  const bool all_feasible = std::all_of(points.cbegin(), points.cend(), [](const AnalysisPoint& point) { return point.feasible && point.deadline_misses == 0; });
  output << std::fixed << std::setprecision(6);
  output << "{\n  \"schema_version\": \"axissim.sensitivity.v1\",\n"
         << "  \"interpretation\": \"Declared sensitivity bounds, not statistical confidence intervals or measured silicon error bars.\",\n"
         << "  \"decision_stability\": \"" << (all_feasible ? "robust within declared sensitivity bounds" : "conditional: feasibility or deadline outcome changes across declared sensitivity bounds") << "\",\n"
         << "  \"ranges\": [\n";
  for (std::size_t index = 0; index < ranges.size(); ++index) {
    const auto& range = ranges[index];
    output << "    {\"parameter\": \"" << json_escape(range.parameter) << "\", \"low\": " << range.low << ", \"nominal\": " << range.nominal
           << ", \"high\": " << range.high << ", \"unit\": \"" << json_escape(range.unit) << "\", \"evidence_class\": \"" << json_escape(range.evidence_class)
           << "\", \"rationale\": \"" << json_escape(range.rationale) << "\"}" << (index + 1 == ranges.size() ? "\n" : ",\n");
  }
  output << "  ],\n  \"summary\": {\"point_count\": " << points.size() << ", \"latency_min_us\": " << min_latency->mean_latency_us
         << ", \"latency_max_us\": " << max_latency->mean_latency_us << ", \"energy_min_uj\": " << min_energy->it_energy_per_frame_uj
         << ", \"energy_max_uj\": " << max_energy->it_energy_per_frame_uj << "}\n}\n";
}

void run_sensitivity_analysis(const std::string& config_path, const std::map<std::string, std::string>& base_overrides,
                              const std::vector<SensitivityRange>& ranges, std::ostream* csv_output, std::ostream* json_output) {
  std::vector<AnalysisPoint> points;
  for (const auto& range : ranges) {
    for (const auto& [name, value] : std::array<std::pair<const char*, double>, 3>{{{"low", range.low}, {"nominal", range.nominal}, {"high", range.high}}}) {
      auto overrides = base_overrides;
      overrides[range.parameter] = format_setting(value);
      points.push_back(evaluate_analysis_point(config_path, overrides, "one_at_a_time", range.parameter, name));
    }
  }
  std::vector<std::map<std::string, std::string>> corners = {base_overrides};
  for (const auto& range : ranges) {
    std::vector<std::map<std::string, std::string>> next;
    for (const auto& corner : corners) {
      for (const double value : {range.low, range.nominal, range.high}) {
        auto expanded = corner;
        expanded[range.parameter] = format_setting(value);
        next.push_back(std::move(expanded));
      }
    }
    corners = std::move(next);
  }
  for (const auto& corner : corners) {
    std::ostringstream setting;
    bool first = true;
    for (const auto& range : ranges) {
      if (!first) setting << ';';
      setting << range.parameter << '=' << corner.at(range.parameter);
      first = false;
    }
    points.push_back(evaluate_analysis_point(config_path, corner, "corner", "all", setting.str()));
  }
  if (csv_output) write_analysis_csv(points, *csv_output);
  if (json_output) write_analysis_json(ranges, points, *json_output);
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

void write_calibration_csv(const std::string& config_path, const std::map<std::string, std::string>& base_overrides,
                           const std::string& reference_path, std::ostream& output) {
  std::ifstream input(reference_path);
  if (!input) throw std::runtime_error("Unable to open reference data: " + reference_path);
  const auto reference_directory = std::filesystem::path(reference_path).parent_path();
  std::string header;
  if (!std::getline(input, header)) throw std::runtime_error("Reference data is empty");
  const auto columns = split_csv_values(header);
  const auto find_column = [&columns](const std::string& name) {
    const auto position = std::find(columns.begin(), columns.end(), name);
    if (position == columns.end()) throw std::runtime_error("Reference data missing column: " + name);
    return static_cast<std::size_t>(position - columns.begin());
  };
  const auto id_column = find_column("reference_id");
  const auto status_column = find_column("status");
  const auto source_url_column = find_column("source_url");
  const auto source_revision_column = find_column("source_revision");
  const auto benchmark_column = find_column("benchmark");
  const auto lanes_column = find_column("lanes");
  const auto frequency_column = find_column("frequency_ghz");
  const auto workload_column = find_column("axis_workload");
  const auto config_column = find_column("axis_config");
  const auto unit_column = find_column("unit");
  const auto extraction_column = find_column("extraction_method");
  const auto latency_column = find_column("reference_latency_us");
  const auto cycle_position = std::find(columns.begin(), columns.end(), "reference_cycles");
  const std::optional<std::size_t> cycle_column = cycle_position == columns.end()
      ? std::nullopt
      : std::optional<std::size_t>(static_cast<std::size_t>(cycle_position - columns.begin()));
  output << "reference_id,status,axis_config,lanes,reference_cycles,predicted_cycles,absolute_error_cycles,relative_error_cycles_percent,reference_latency_us,predicted_latency_us,absolute_error_us,relative_error_percent\n";
  output << std::fixed << std::setprecision(3);
  std::string line;
  while (std::getline(input, line)) {
    if (trim(line).empty() || trim(line).starts_with('#')) continue;
    const auto row = split_csv_values(line);
    if (row.size() != columns.size()) throw std::runtime_error("Reference row has wrong column count");
    for (const auto column : {id_column, source_url_column, source_revision_column, benchmark_column, lanes_column,
                              frequency_column, workload_column, config_column, unit_column, extraction_column, latency_column}) {
      if (row[column].empty()) throw std::runtime_error("Reference row has an empty provenance field");
    }
    if (row[status_column] != "published" && row[status_column] != "reproduced") {
      throw std::runtime_error("Reference status must be published or reproduced");
    }
    if (row[unit_column] != "us") throw std::runtime_error("Reference latency unit must be us");
    if (std::stoi(row[lanes_column]) <= 0 || std::stod(row[frequency_column]) <= 0.0 || std::stod(row[latency_column]) <= 0.0) {
      throw std::runtime_error("Reference lanes, frequency, and latency must be positive");
    }
    if (cycle_column && !row[*cycle_column].empty() && std::stod(row[*cycle_column]) <= 0.0) {
      throw std::runtime_error("Reference cycles must be positive when supplied");
    }
    auto overrides = base_overrides;
    overrides["rvv_lanes"] = row[lanes_column];
    const auto axis_config = row[config_column] == "." ? config_path : (reference_directory / row[config_column]).lexically_normal().string();
    const Config config = load_config(axis_config, overrides);
    validate(config);
    const double reference_frequency = std::stod(row[frequency_column]);
    if (std::abs(config.rvv_frequency_ghz - reference_frequency) > 1e-12) {
      throw std::runtime_error("Reference frequency does not match the selected AxisSim configuration");
    }
    Simulator simulator(config);
    simulator.run();
    const auto& latencies = simulator.latencies_us();
    const double predicted = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    const double reference = std::stod(row[latency_column]);
    const double absolute_error = std::abs(predicted - reference);
    const double relative_error = reference == 0.0 ? 0.0 : 100.0 * absolute_error / std::abs(reference);
    output << row[id_column] << ',' << row[status_column] << ',' << row[config_column] << ',' << row[lanes_column] << ',';
    if (cycle_column && !row[*cycle_column].empty()) {
      if (config.workload_mode != "rvv_stream") throw std::runtime_error("Reference cycles require workload_mode=rvv_stream");
      const double reference_cycles = std::stod(row[*cycle_column]);
      const double predicted_cycles = simulator.rvv_stream_compute_cycles();
      const double absolute_cycle_error = std::abs(predicted_cycles - reference_cycles);
      const double relative_cycle_error = 100.0 * absolute_cycle_error / std::abs(reference_cycles);
      output << reference_cycles << ',' << predicted_cycles << ',' << absolute_cycle_error << ',' << relative_cycle_error << ',';
    } else {
      output << ",,,,";
    }
    output << reference << ',' << predicted << ',' << absolute_error << ',' << relative_error << '\n';
  }
}

}  // namespace axissim

int main(int argc, char** argv) {
  using namespace axissim;
  try {
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "profile-validate") {
      const WorkloadProfile profile = load_workload_profile(argv[2]);
      if (argc == 4) (void)load_mapping_plan(argv[3], profile);
      std::cout << "Workload profile is valid: " << argv[2] << " (" << profile.operators.size() << " operators; source " << profile.source_url << ")";
      if (argc == 4) std::cout << "; mapping plan valid: " << argv[3];
      std::cout << '\n';
      return 0;
    }
    if (argc < 3) {
      std::cerr << "Usage: axissim <validate|run|sweep|calibrate|analyze> <config.conf> [options]\n"
                   "       axissim profile-validate <workload-profile.json> [mapping-plan.json]\n";
      return 2;
    }
    const std::string command = argv[1];
    std::optional<std::string> trace_path;
    std::optional<std::string> json_path;
    std::optional<std::string> output_path;
    std::optional<std::string> sweep_assignment;
    std::optional<std::string> reference_path;
    std::optional<std::string> ranges_path;
    std::map<std::string, std::string> overrides;
    for (int argument = 3; argument < argc; ++argument) {
      const std::string option = argv[argument];
      if (option == "--trace") {
        if (++argument >= argc || trace_path.has_value()) throw std::runtime_error("--trace requires one path and may appear once");
        trace_path = argv[argument];
      } else if (option == "--json") {
        if (++argument >= argc || json_path.has_value()) throw std::runtime_error("--json requires one path and may appear once");
        json_path = argv[argument];
      } else if (option == "--csv") {
        if (++argument >= argc || output_path.has_value()) throw std::runtime_error("--csv requires one path and may appear once");
        output_path = argv[argument];
      } else if (option == "--sweep") {
        if (++argument >= argc || sweep_assignment.has_value()) throw std::runtime_error("--sweep requires one key=value1,value2,... assignment and may appear once");
        sweep_assignment = argv[argument];
      } else if (option == "--reference") {
        if (++argument >= argc || reference_path.has_value()) throw std::runtime_error("--reference requires one CSV path and may appear once");
        reference_path = argv[argument];
      } else if (option == "--ranges") {
        if (++argument >= argc || ranges_path.has_value()) throw std::runtime_error("--ranges requires one CSV path and may appear once");
        ranges_path = argv[argument];
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
      if (!sweep_assignment || trace_path || json_path) throw std::runtime_error("sweep requires --sweep and does not support --trace or --json");
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
    if (command == "calibrate") {
      if (!reference_path || trace_path || json_path || sweep_assignment) throw std::runtime_error("calibrate requires --reference and does not support --trace, --json, or --sweep");
      if (output_path) {
        std::ofstream output(*output_path);
        if (!output) throw std::runtime_error("Unable to write CSV: " + *output_path);
        write_calibration_csv(argv[2], overrides, *reference_path, output);
      } else {
        write_calibration_csv(argv[2], overrides, *reference_path, std::cout);
      }
      return 0;
    }
    if (command == "analyze") {
      if (!ranges_path || trace_path || sweep_assignment || reference_path || (!output_path && !json_path)) {
        throw std::runtime_error("analyze requires --ranges and at least one of --csv or --json; it does not support --trace, --sweep, or --reference");
      }
      const auto ranges = read_sensitivity_ranges(*ranges_path);
      std::optional<std::ofstream> csv;
      std::optional<std::ofstream> json;
      if (output_path) {
        csv.emplace(*output_path);
        if (!*csv) throw std::runtime_error("Unable to write CSV: " + *output_path);
      }
      if (json_path) {
        json.emplace(*json_path);
        if (!*json) throw std::runtime_error("Unable to write JSON: " + *json_path);
      }
      run_sensitivity_analysis(argv[2], overrides, ranges, csv ? &*csv : nullptr, json ? &*json : nullptr);
      if (output_path) std::cout << "Wrote sensitivity points to " << *output_path << '\n';
      if (json_path) std::cout << "Wrote sensitivity summary to " << *json_path << '\n';
      return 0;
    }
    if (output_path || sweep_assignment || reference_path || ranges_path) throw std::runtime_error("--csv, --sweep, --reference, and --ranges are only valid with their matching commands");
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
    if (json_path) {
      write_result_json(*json_path, simulator, config);
      std::cout << "  result JSON:         " << *json_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
