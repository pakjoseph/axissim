#include "run_provenance.hpp"

#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

#include "build_info.hpp"
#include "gemm_json.hpp"
#include "profile.hpp"
#include "sha256.hpp"

namespace axissim {
namespace {

using gemm_json::Array;
using gemm_json::Object;
using gemm_json::Value;

void append_quoted(std::string& output, const std::string& text) {
  constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char byte : text) {
    if (byte == '"' || byte == '\\') {
      output.push_back('\\');
      output.push_back(static_cast<char>(byte));
    } else if (byte < 0x20) {
      output += "\\u00";
      output.push_back(hex[byte >> 4]);
      output.push_back(hex[byte & 0xf]);
    } else {
      output.push_back(static_cast<char>(byte));
    }
  }
  output.push_back('"');
}

void append_canonical(std::string& output, const Value& value) {
  if (value.is_null()) {
    output += "null";
  } else if (value.is_bool()) {
    output += std::get<bool>(value.storage) ? "true" : "false";
  } else if (value.is_number()) {
    output += std::get<gemm_json::Number>(value.storage).lexeme;
  } else if (value.is_string()) {
    append_quoted(output, value.string());
  } else if (value.is_array()) {
    output.push_back('[');
    bool first = true;
    for (const auto& child : value.array()) {
      if (!first) output.push_back(',');
      first = false;
      append_canonical(output, child);
    }
    output.push_back(']');
  } else {
    output.push_back('{');
    bool first = true;
    for (const auto& [key, child] : value.object()) {
      if (!first) output.push_back(',');
      first = false;
      append_quoted(output, key);
      output.push_back(':');
      append_canonical(output, child);
    }
    output.push_back('}');
  }
}

[[nodiscard]] std::string canonical_value(const Value& value) {
  std::string output;
  append_canonical(output, value);
  return output;
}

[[nodiscard]] Value number(double value) {
  if (!std::isfinite(value)) return Value("unknown");
  char buffer[128]{};
  const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                       std::chars_format::general,
                                       std::numeric_limits<double>::max_digits10);
  if (converted.ec != std::errc{}) return Value("unknown");
  return Value(gemm_json::Number{std::string(buffer, converted.ptr)});
}

[[nodiscard]] Value integer(std::int64_t value) {
  return Value(gemm_json::Number{std::to_string(value)});
}

[[nodiscard]] Value unsigned_integer(std::uint64_t value) {
  return Value(value);
}

[[nodiscard]] std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to read provenance input: " + path.filename().string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string relative_identity_path(const std::filesystem::path& path,
                                                 const std::filesystem::path& root) {
  const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() || relative.is_absolute()) return "external/" + path.filename().generic_string();
  const auto text = relative.generic_string();
  if (text == ".." || text.starts_with("../")) return "external/" + path.filename().generic_string();
  return text;
}

[[nodiscard]] std::string resolved_path_identity(const std::string& value,
                                                 const std::filesystem::path& root) {
  if (value.empty()) return "";
  const std::filesystem::path authored(value);
  // Config:: paths have already been prefixed with the root manifest's
  // directory by the authoritative parser. A relative result is therefore
  // relative to the process working directory, not to the manifest again.
  const auto resolved = authored.is_absolute() ? authored : std::filesystem::absolute(authored);
  return relative_identity_path(resolved, root);
}

[[nodiscard]] std::string authored_path_identity(const std::string& value,
                                                 const std::filesystem::path& root) {
  if (value.empty()) return "";
  const std::filesystem::path authored(value);
  const auto resolved = authored.is_absolute() ? authored : root / authored;
  return relative_identity_path(resolved, root);
}

struct InputRecord {
  std::string role;
  std::string path;
  std::string hash;
  bool available = false;
};

[[nodiscard]] Value input_record(const InputRecord& record) {
  return Object{{"path", Value(record.path)},
                {"role", Value(record.role)},
                {"sha256", Value(record.available ? record.hash : "unknown")},
                {"status", Value(record.available ? "hashed" : "unavailable")}};
}

[[nodiscard]] std::vector<InputRecord> collect_inputs(
    const ConfigLoadResult& loaded, const std::filesystem::path& root_manifest) {
  std::vector<InputRecord> records;
  const auto root = std::filesystem::absolute(root_manifest).lexically_normal();
  const auto base = root.parent_path();
  for (std::size_t index = 0; index < loaded.source_files.size(); ++index) {
    const auto& file = loaded.source_files[index];
    const auto contents = loaded.source_contents.find(file);
    records.push_back({index == 0 ? "root_configuration" : "included_configuration",
                       relative_identity_path(file, base),
                       contents == loaded.source_contents.end() ? "" : sha256_hex(contents->second),
                       contents != loaded.source_contents.end()});
  }

  const auto add_file = [&records, &base](const std::filesystem::path& file,
                                         const std::string& role) {
    if (file.empty()) return;
    const auto resolved_file = file.is_absolute() ? file : std::filesystem::absolute(file).lexically_normal();
    try {
      const auto contents = read_bytes(resolved_file);
      records.push_back({role, relative_identity_path(resolved_file, base), sha256_hex(contents), true});
    } catch (const std::exception&) {
      records.push_back({role, relative_identity_path(resolved_file, base), "", false});
    }
  };

  const auto& config = loaded.config;
  if (!config.workload_profile.empty()) {
    add_file(config.workload_profile, "workload_profile");
    if (!config.mapping_plan.empty()) {
      add_file(config.mapping_plan, "mapping_plan");
      try {
        const auto profile = load_workload_profile(config.workload_profile);
        const auto plan = load_mapping_plan(config.mapping_plan, profile);
        if (!plan.accuracy_evidence_path.empty())
          add_file(plan.accuracy_evidence_path, "mapping_accuracy_evidence");
      } catch (const std::exception&) {
        // The successful simulator run is the authority. A second-pass parse
        // failure or changed file is reflected as incomplete provenance.
        records.push_back({"mapping_resolution", relative_identity_path(config.mapping_plan, base), "", false});
      }
    }
  }
  if (!config.frame_arrival_trace.empty())
    add_file(config.frame_arrival_trace, "frame_arrival_trace");
  if (!config.frame_arrival_provenance.empty())
    add_file(config.frame_arrival_provenance, "frame_arrival_provenance");
  return records;
}

[[nodiscard]] Value config_json(const Config& c, const std::filesystem::path& config_root) {
  Object out;
#define AXIS_STRING(field) out.emplace(#field, Value(c.field))
#define AXIS_INT(field) out.emplace(#field, integer(c.field))
#define AXIS_UINT(field) out.emplace(#field, unsigned_integer(c.field))
#define AXIS_DOUBLE(field) out.emplace(#field, number(c.field))
  AXIS_STRING(scenario_name); AXIS_STRING(model_version); AXIS_STRING(workload_mode);
  out.emplace("workload_profile", Value(resolved_path_identity(c.workload_profile, config_root)));
  out.emplace("mapping_plan", Value(resolved_path_identity(c.mapping_plan, config_root)));
  AXIS_STRING(scheduler_policy);
  AXIS_STRING(scheduler_ordering_policy); AXIS_STRING(frame_arrival_mode);
  out.emplace("frame_arrival_trace", Value(resolved_path_identity(c.frame_arrival_trace, config_root)));
  out.emplace("frame_arrival_provenance", Value(resolved_path_identity(c.frame_arrival_provenance, config_root)));
  AXIS_INT(frames); AXIS_DOUBLE(frame_period_us); AXIS_DOUBLE(deadline_us);
  AXIS_DOUBLE(dram_bandwidth_gbps); AXIS_DOUBLE(dma_setup_us);
  AXIS_DOUBLE(control_period_us); AXIS_DOUBLE(control_deadline_us); AXIS_UINT(control_ops);
  AXIS_INT(rvv_vlen_bits); AXIS_INT(rvv_sew_bits); AXIS_INT(rvv_lmul); AXIS_INT(rvv_lanes);
  AXIS_INT(rvv_ops_per_lane_cycle); AXIS_DOUBLE(rvv_frequency_ghz);
  AXIS_DOUBLE(rvv_stripmine_overhead_cycles); AXIS_STRING(rvv_timing_model);
  AXIS_DOUBLE(rvv_affine_overhead_cycles); AXIS_DOUBLE(rvv_compute_cycle_scale);
  AXIS_UINT(rvv_chunk_elements); AXIS_STRING(rvv_memory_accounting); AXIS_INT(rvv_memory_bytes_per_element);
  AXIS_INT(npu_macs); AXIS_DOUBLE(npu_frequency_ghz); AXIS_UINT(input_bytes);
  AXIS_UINT(preprocess_elements); AXIS_INT(preprocess_ops_per_element); AXIS_UINT(preprocess_output_bytes);
  AXIS_INT(gemm_m); AXIS_INT(gemm_n); AXIS_INT(gemm_k); AXIS_INT(npu_tile_m);
  AXIS_INT(npu_tile_n); AXIS_INT(npu_tile_k); AXIS_INT(npu_element_bytes);
  AXIS_INT(npu_accumulator_bytes); AXIS_UINT(sram_capacity_bytes); AXIS_INT(npu_weights_resident);
  AXIS_UINT(weight_sram_capacity_bytes); AXIS_INT(dma_double_buffering);
  AXIS_UINT(postprocess_elements); AXIS_INT(postprocess_ops_per_element); AXIS_UINT(postprocess_bytes);
  AXIS_DOUBLE(vector_op_pj); AXIS_DOUBLE(npu_mac_pj); AXIS_DOUBLE(npu_arithmetic_energy_scale);
  AXIS_DOUBLE(arithmetic_voltage_ratio); AXIS_DOUBLE(dram_byte_pj); AXIS_DOUBLE(dma_byte_pj);
  AXIS_DOUBLE(static_power_w); AXIS_DOUBLE(npu_static_power_per_mac_w);
  AXIS_DOUBLE(rvv_static_power_per_lane_w); AXIS_DOUBLE(sram_static_power_per_kib_w);
  AXIS_DOUBLE(dram_interface_static_power_w); AXIS_DOUBLE(it_power_budget_w);
  AXIS_DOUBLE(pue); AXIS_DOUBLE(electricity_cost_per_kwh); AXIS_INT(deployment_xpu_count);
  AXIS_DOUBLE(deployment_hours); AXIS_DOUBLE(base_area_mm2); AXIS_DOUBLE(npu_area_per_mac_mm2);
  AXIS_DOUBLE(rvv_area_per_lane_mm2); AXIS_DOUBLE(sram_area_per_kib_mm2); AXIS_DOUBLE(max_area_mm2);
  AXIS_DOUBLE(ambient_temp_c); AXIS_DOUBLE(thermal_resistance_c_per_w);
  AXIS_DOUBLE(thermal_capacitance_j_per_c); AXIS_DOUBLE(max_junction_temp_c);
  AXIS_DOUBLE(reliability_fit_rate); AXIS_DOUBLE(reliability_voltage_fit_multiplier);
#undef AXIS_STRING
#undef AXIS_INT
#undef AXIS_UINT
#undef AXIS_DOUBLE
  return out;
}

[[nodiscard]] Value make_identity_payload(const ConfigLoadResult& loaded,
                                          const std::filesystem::path& root_manifest,
                                          const std::filesystem::path& executable,
                                          bool* complete) {
  const auto inputs = collect_inputs(loaded, root_manifest);
  Array input_array;
  for (const auto& input : inputs) {
    if (!input.available) *complete = false;
    input_array.push_back(input_record(input));
  }
  if (loaded.source_files.empty()) *complete = false;

  std::string executable_hash = "unknown";
  std::string executable_status = "unavailable";
  try {
    executable_hash = sha256_hex(read_bytes(executable));
    executable_status = "hashed";
  } catch (const std::exception&) {
    *complete = false;
  }

  const std::string source_revision = AXISSIM_SOURCE_REVISION;
  const std::string source_state = AXISSIM_SOURCE_TREE_STATE;
  const std::string compiler_id = AXISSIM_COMPILER_ID;
  const std::string compiler_version = AXISSIM_COMPILER_VERSION;
  if (source_revision == "unknown" || source_state != "clean" ||
      compiler_id == "unknown" || compiler_version == "unknown") *complete = false;

  Object overrides;
  const auto config_root = std::filesystem::absolute(root_manifest).lexically_normal().parent_path();
  const auto path_key = [](const std::string& key) {
    return key == "workload_profile" || key == "mapping_plan" ||
           key == "frame_arrival_trace" || key == "frame_arrival_provenance";
  };
  for (const auto& [key, value] : loaded.explicit_overrides)
    overrides.emplace(key, Value(path_key(key) ? authored_path_identity(value, config_root) : value));
  Object authored_values;
  for (const auto& [key, value] : loaded.resolved_values)
    authored_values.emplace(key, Value(path_key(key) ? authored_path_identity(value, config_root) : value));
  const auto absolute_manifest = std::filesystem::absolute(root_manifest).lexically_normal();
  const auto manifest_id = relative_identity_path(absolute_manifest, config_root);
  const Value& root_file = input_array.empty() ? Value("unknown") : input_array.front();

  return Object{
      {"build", Object{{"build_type", Value(AXISSIM_BUILD_TYPE)},
                        {"cmake_version", Value(AXISSIM_CMAKE_VERSION)},
                        {"compiler_id", Value(compiler_id)},
                        {"compiler_version", Value(compiler_version)},
                        {"cxx_standard", Value(AXISSIM_CXX_STANDARD)},
                        {"platform", Value(AXISSIM_TARGET_PLATFORM)},
                        {"qt_installation_sbom_status", Value(AXISSIM_QT_SBOM_STATUS)},
                        {"qt_runtime_mode", Value(AXISSIM_QT_RUNTIME_MODE)},
                        {"qt_version", Value(AXISSIM_QT_VERSION)}}},
      {"authored_values_after_overrides", Value(std::move(authored_values))},
      {"configuration", config_json(loaded.config, config_root)},
      {"configuration_files", Value(std::move(input_array))},
      {"input_verification", Object{{"status", Value("pending")}}},
      {"explicit_cli_overrides", Value(std::move(overrides))},
      {"executable", Object{{"sha256", Value(executable_hash)}, {"status", Value(executable_status)}}},
      {"manifest", Object{{"path", Value(manifest_id)}, {"identity", root_file}}},
      {"package", Object{{"kind", Value(AXISSIM_PACKAGE_KIND)},
                          {"id", Value(AXISSIM_PACKAGE_ID)},
                          {"release_tag", Value(AXISSIM_RELEASE_TAG)}}},
      {"scenario", Object{{"name", Value(loaded.config.scenario_name)},
                           {"model_version", Value(loaded.config.model_version)}}},
      {"source", Object{{"revision", Value(source_revision)},
                         {"tree_state", Value(source_state)}}},
      {"schema_version", Value("axissim.run-provenance.v1")}};
}

[[nodiscard]] Value identity_record(Value payload, bool complete) {
  const Value status(complete ? "complete" : "incomplete");
  const Value identity_input(Object{{"identity_status", status}, {"provenance", payload}});
  return Object{{"identity_status", status},
                {"identity_sha256", Value(sha256_hex(canonical_value(identity_input)))},
                {"provenance", std::move(payload)}};
}

[[nodiscard]] bool input_snapshot_unchanged(
    const Value& input_snapshot, const ConfigLoadResult& loaded,
    const std::filesystem::path& root_manifest) {
  try {
    const auto& original_payload = input_snapshot.object().at("provenance");
    const auto& original_inputs = original_payload.object().at("configuration_files").array();
    const auto current_inputs = collect_inputs(loaded, root_manifest);
    using Key = std::pair<std::string, std::string>;
    std::map<Key, std::pair<std::string, std::string>> current;
    for (const auto& input : current_inputs) {
      current[{input.role, input.path}] = {
          input.available ? input.hash : "unknown",
          input.available ? "hashed" : "unavailable"};
    }
    if (current.size() != original_inputs.size()) return false;
    for (const auto& input : original_inputs) {
      const auto& fields = input.object();
      const Key key{fields.at("role").string(), fields.at("path").string()};
      const auto found = current.find(key);
      if (found == current.end() ||
          found->second.first != fields.at("sha256").string() ||
          found->second.second != fields.at("status").string()) return false;
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

Value make_run_provenance(const ConfigLoadResult& loaded,
                          const std::filesystem::path& root_manifest,
                          const std::filesystem::path& executable) {
  bool complete = true;
  auto payload = make_identity_payload(loaded, root_manifest, executable, &complete);
  return identity_record(std::move(payload), complete);
}

Value finalize_run_provenance(const Value& input_snapshot,
                              const ConfigLoadResult& loaded,
                              const std::filesystem::path& root_manifest,
                              const std::filesystem::path& result_json_path) {
  const auto& snapshot_record = input_snapshot.object();
  bool complete = snapshot_record.at("identity_status").string() == "complete";
  auto payload = snapshot_record.at("provenance");
  auto& fields = std::get<Object>(payload.storage);
  const bool inputs_unchanged = input_snapshot_unchanged(input_snapshot, loaded, root_manifest);
  fields["input_verification"] = Object{{"status", Value(inputs_unchanged ? "unchanged" : "changed_or_unavailable")}};
  if (!inputs_unchanged) complete = false;

  try {
    const auto result_bytes = read_bytes(result_json_path);
    fields["result"] = Object{{"sha256", Value(sha256_hex(result_bytes))}, {"status", Value("hashed")}};
  } catch (const std::exception&) {
    fields["result"] = Object{{"sha256", Value("unknown")}, {"status", Value("unavailable")}};
    complete = false;
  }
  return identity_record(std::move(payload), complete);
}

std::string canonical_provenance_value(const Value& value) {
  return canonical_value(value);
}

std::string canonical_run_provenance(const ConfigLoadResult& loaded,
                                     const std::filesystem::path& root_manifest,
                                     const std::filesystem::path& executable) {
  return canonical_value(make_run_provenance(loaded, root_manifest, executable));
}

void write_run_provenance(const std::filesystem::path& result_json_path,
                          const ConfigLoadResult& loaded,
                          const std::filesystem::path& root_manifest,
                          const std::filesystem::path& executable) {
  const auto snapshot = make_run_provenance(loaded, root_manifest, executable);
  write_run_provenance_record(result_json_path,
      finalize_run_provenance(snapshot, loaded, root_manifest, result_json_path));
}

void write_run_provenance_record(const std::filesystem::path& result_json_path,
                                 const Value& provenance_record) {
  auto sidecar = result_json_path;
  sidecar += ".provenance.json";
  std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Unable to write run provenance sidecar: " + sidecar.filename().string());
  output << canonical_provenance_value(provenance_record) << '\n';
  if (!output) throw std::runtime_error("Unable to finish run provenance sidecar: " + sidecar.filename().string());
}

}  // namespace axissim
