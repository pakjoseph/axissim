#include "run_provenance.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
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
    const ConfigLoadResult& loaded, const std::filesystem::path& root_manifest,
    const std::vector<CapturedRuntimeInput>& captured_inputs) {
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

  for (const auto& input : captured_inputs) {
    records.push_back({input.role, relative_identity_path(input.path, base),
                       sha256_hex(input.bytes), true});
  }
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
                                          const std::vector<CapturedRuntimeInput>& captured_inputs,
                                          bool* complete) {
  const auto inputs = collect_inputs(loaded, root_manifest, captured_inputs);
  Array input_array;
  for (const auto& input : inputs) {
    if (!input.available) *complete = false;
    input_array.push_back(input_record(input));
  }
  if (loaded.source_files.empty()) *complete = false;
  const auto has_captured = [&captured_inputs](const std::string& role) {
    return std::any_of(captured_inputs.begin(), captured_inputs.end(), [&role](const auto& input) {
      return input.role == role;
    });
  };
  const auto& config = loaded.config;
  if (config.frame_arrival_mode == "trace" &&
      (!has_captured("frame_arrival_trace") || !has_captured("frame_arrival_provenance")))
    *complete = false;
  if (config.workload_mode == "profiled_graph" && !config.workload_profile.empty() &&
      !has_captured("workload_profile")) *complete = false;
  if (config.workload_mode == "profiled_graph" && !config.mapping_plan.empty() &&
      !has_captured("mapping_plan")) *complete = false;
  if (config.workload_mode == "profiled_graph" && !config.mapping_plan.empty()) {
    const auto mapping = std::find_if(captured_inputs.begin(), captured_inputs.end(), [](const auto& input) {
      return input.role == "mapping_plan";
    });
    if (mapping != captured_inputs.end() &&
        mapping_plan_accuracy_evidence_path(mapping->path, mapping->bytes) &&
        !has_captured("mapping_accuracy_evidence")) *complete = false;
  }

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
      {"required_runtime_inputs", [&captured_inputs, &config_root]() {
        Array requirements;
        for (const auto& input : captured_inputs) {
          requirements.emplace_back(Object{
              {"path", Value(relative_identity_path(input.path, config_root))},
              {"role", Value(input.role)}});
        }
        return Value(std::move(requirements));
      }()},
      {"input_verification", Object{{"status", Value("exact_bytes_captured_and_used")}}},
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
      {"schema_version", Value("axissim.run-provenance.v2")}};
}

[[nodiscard]] Value identity_record(Value payload, bool complete) {
  const Value status(complete ? "complete" : "incomplete");
  const Value identity_input(Object{{"identity_status", status}, {"provenance", payload}});
  return Object{{"identity_status", status},
                {"identity_sha256", Value(sha256_hex(canonical_value(identity_input)))},
                {"provenance", std::move(payload)}};
}

}  // namespace

Value make_run_provenance(const ConfigLoadResult& loaded,
                          const std::filesystem::path& root_manifest,
                          const std::filesystem::path& executable,
                          const std::vector<CapturedRuntimeInput>& captured_inputs) {
  bool complete = true;
  auto payload = make_identity_payload(loaded, root_manifest, executable, captured_inputs, &complete);
  return identity_record(std::move(payload), complete);
}

Value finalize_run_provenance(const Value& input_snapshot,
                              const std::filesystem::path& result_json_path) {
  const auto& snapshot_record = input_snapshot.object();
  bool complete = snapshot_record.at("identity_status").string() == "complete";
  auto payload = snapshot_record.at("provenance");
  auto& fields = std::get<Object>(payload.storage);
  if (fields.at("input_verification").object().at("status").string() !=
      "exact_bytes_captured_and_used") complete = false;

  try {
    const auto result_bytes = read_bytes(result_json_path);
    fields["result"] = Object{{"sha256", Value(sha256_hex(result_bytes))}, {"status", Value("hashed")}};
  } catch (const std::exception&) {
    fields["result"] = Object{{"sha256", Value("unknown")}, {"status", Value("unavailable")}};
    complete = false;
  }
  return identity_record(std::move(payload), complete);
}

std::optional<std::string> run_provenance_completeness_issue(const Value& provenance_record) {
  const auto* top = std::get_if<Object>(&provenance_record.storage);
  if (top == nullptr) return "run provenance record is not an object";
  const auto status_it = top->find("identity_status");
  if (status_it == top->end() || !status_it->second.is_string())
    return "run provenance identity status is missing";
  if (status_it->second.string() != "complete") return std::nullopt;
  const auto payload_it = top->find("provenance");
  if (payload_it == top->end() || !payload_it->second.is_object())
    return "complete run provenance payload is missing";
  const auto& payload = payload_it->second.object();
  const auto field = [](const Object& object, const std::string& key) -> const Value* {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
  };
  const auto object_field = [&field](const Object& object, const std::string& key) -> const Object* {
    const auto* value = field(object, key);
    return value != nullptr && value->is_object() ? &value->object() : nullptr;
  };
  const auto string_field = [&field](const Object& object, const std::string& key) -> std::string {
    const auto* value = field(object, key);
    return value != nullptr && value->is_string() ? value->string() : std::string{};
  };
  const auto known = [](const std::string& value) {
    return !value.empty() && value != "unknown";
  };
  const auto is_sha256 = [](const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  };
  const auto is_revision = [](const std::string& value) {
    return (value.size() == 40 || value.size() == 64) && std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  };

  const auto* source = object_field(payload, "source");
  if (source == nullptr || !is_revision(string_field(*source, "revision")))
    return "complete run provenance has no valid source revision";
  if (string_field(*source, "tree_state") != "clean")
    return "complete run provenance does not identify a clean source tree";

  const auto* build = object_field(payload, "build");
  if (build == nullptr || !known(string_field(*build, "compiler_id")) ||
      !known(string_field(*build, "compiler_version")) ||
      !known(string_field(*build, "build_type")) ||
      !known(string_field(*build, "platform")) ||
      !known(string_field(*build, "cmake_version")))
    return "complete run provenance has incomplete build/toolchain identity";
  if (string_field(*build, "cxx_standard") != "20")
    return "complete run provenance has an unsupported C++ standard identity";

  const auto* executable = object_field(payload, "executable");
  if (executable == nullptr || string_field(*executable, "status") != "hashed" ||
      !is_sha256(string_field(*executable, "sha256")))
    return "complete run provenance has no verified executable hash";

  const auto* manifest = object_field(payload, "manifest");
  const auto* manifest_identity = manifest == nullptr ? nullptr : object_field(*manifest, "identity");
  if (manifest == nullptr || !known(string_field(*manifest, "path")) ||
      manifest_identity == nullptr || string_field(*manifest_identity, "role") != "root_configuration" ||
      string_field(*manifest_identity, "status") != "hashed" ||
      !is_sha256(string_field(*manifest_identity, "sha256")))
    return "complete run provenance has no hashed root configuration manifest";

  const auto* files_value = field(payload, "configuration_files");
  if (files_value == nullptr || !files_value->is_array() || files_value->array().empty())
    return "complete run provenance has no configuration file inventory";
  const Object* root_file = nullptr;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> runtime_files;
  for (const auto& item : files_value->array()) {
    if (!item.is_object()) return "complete run provenance has a malformed input record";
    const auto& entry = item.object();
    if (!known(string_field(entry, "role")) || !known(string_field(entry, "path")) ||
        string_field(entry, "status") != "hashed" || !is_sha256(string_field(entry, "sha256")))
      return "complete run provenance has an unavailable or malformed input hash";
    if (string_field(entry, "role") == "root_configuration") {
      if (root_file != nullptr) return "complete run provenance has multiple root configuration records";
      root_file = &entry;
    } else if (string_field(entry, "role") == "frame_arrival_trace" ||
               string_field(entry, "role") == "frame_arrival_provenance" ||
               string_field(entry, "role") == "workload_profile" ||
               string_field(entry, "role") == "mapping_plan" ||
               string_field(entry, "role") == "mapping_accuracy_evidence") {
      runtime_files[string_field(entry, "role")].emplace_back(
          string_field(entry, "path"), string_field(entry, "sha256"));
    } else if (string_field(entry, "role") != "included_configuration") {
      return "complete run provenance has an unsupported input role";
    }
  }
  if (root_file == nullptr || string_field(*root_file, "path") != string_field(*manifest_identity, "path") ||
      string_field(*root_file, "sha256") != string_field(*manifest_identity, "sha256"))
    return "complete run provenance root manifest does not match its input inventory";

  const auto* config = object_field(payload, "configuration");
  const auto schema = string_field(payload, "schema_version");
  constexpr std::string_view required_config_fields[] = {
      "ambient_temp_c", "arithmetic_voltage_ratio", "base_area_mm2", "control_deadline_us",
      "control_ops", "control_period_us", "deadline_us", "deployment_hours", "deployment_xpu_count",
      "dma_byte_pj", "dma_double_buffering", "dma_setup_us", "dram_bandwidth_gbps", "dram_byte_pj",
      "dram_interface_static_power_w", "electricity_cost_per_kwh", "frame_arrival_mode",
      "frame_arrival_provenance", "frame_arrival_trace", "frame_period_us", "frames", "gemm_k",
      "gemm_m", "gemm_n", "input_bytes", "it_power_budget_w", "mapping_plan", "max_area_mm2",
      "max_junction_temp_c", "model_version", "npu_accumulator_bytes", "npu_area_per_mac_mm2",
      "npu_arithmetic_energy_scale", "npu_element_bytes", "npu_frequency_ghz", "npu_mac_pj", "npu_macs",
      "npu_static_power_per_mac_w", "npu_tile_k", "npu_tile_m", "npu_tile_n", "npu_weights_resident",
      "postprocess_bytes", "postprocess_elements", "postprocess_ops_per_element", "preprocess_elements",
      "preprocess_ops_per_element", "preprocess_output_bytes", "pue", "reliability_fit_rate",
      "reliability_voltage_fit_multiplier", "rvv_affine_overhead_cycles", "rvv_area_per_lane_mm2",
      "rvv_chunk_elements", "rvv_compute_cycle_scale", "rvv_frequency_ghz", "rvv_lanes", "rvv_lmul",
      "rvv_memory_accounting", "rvv_memory_bytes_per_element", "rvv_ops_per_lane_cycle", "rvv_sew_bits",
      "rvv_static_power_per_lane_w", "rvv_stripmine_overhead_cycles", "rvv_timing_model", "rvv_vlen_bits",
      "scenario_name", "scheduler_ordering_policy", "scheduler_policy", "sram_area_per_kib_mm2",
      "sram_capacity_bytes", "sram_static_power_per_kib_w", "static_power_w", "thermal_capacitance_j_per_c",
      "thermal_resistance_c_per_w", "vector_op_pj", "weight_sram_capacity_bytes", "workload_mode",
      "workload_profile"};
  constexpr std::string_view config_string_fields[] = {
      "scenario_name", "model_version", "workload_mode", "workload_profile", "mapping_plan",
      "scheduler_policy", "scheduler_ordering_policy", "frame_arrival_mode", "frame_arrival_trace",
      "frame_arrival_provenance", "rvv_timing_model", "rvv_memory_accounting"};
  if (config == nullptr || std::any_of(std::begin(required_config_fields), std::end(required_config_fields),
      [&field, config](std::string_view key) { return field(*config, std::string(key)) == nullptr; }))
    return "complete run provenance lacks resolved architecture or timing configuration";
  for (const auto key : config_string_fields) {
    const auto* value = field(*config, std::string(key));
    if (value == nullptr || !value->is_string())
      return "complete run provenance has invalid resolved configuration string fields";
  }
  constexpr std::string_view config_signed_integer_fields[] = {
      "frames", "rvv_vlen_bits", "rvv_sew_bits", "rvv_lmul", "rvv_lanes", "rvv_ops_per_lane_cycle",
      "npu_macs", "preprocess_ops_per_element", "gemm_m", "gemm_n", "gemm_k", "npu_tile_m",
      "npu_tile_n", "npu_tile_k", "npu_element_bytes", "npu_accumulator_bytes", "npu_weights_resident",
      "dma_double_buffering", "rvv_memory_bytes_per_element", "postprocess_ops_per_element",
      "deployment_xpu_count"};
  constexpr std::string_view config_unsigned_integer_fields[] = {
      "control_ops", "rvv_chunk_elements", "input_bytes", "preprocess_elements",
      "preprocess_output_bytes", "sram_capacity_bytes", "weight_sram_capacity_bytes",
      "postprocess_elements", "postprocess_bytes"};
  const auto integer_lexeme_is = [&field](const Object& object, std::string_view key, bool unsigned_only) {
    const auto* value = field(object, std::string(key));
    if (value == nullptr || !value->is_number()) return false;
    const auto& lexeme = std::get<gemm_json::Number>(value->storage).lexeme;
    if (unsigned_only) {
      std::uint64_t parsed = 0;
      const auto [end, error] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), parsed);
      return error == std::errc{} && end == lexeme.data() + lexeme.size();
    }
    int parsed = 0;
    const auto [end, error] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), parsed);
    return error == std::errc{} && end == lexeme.data() + lexeme.size();
  };
  for (const auto key : config_signed_integer_fields) {
    if (!integer_lexeme_is(*config, key, false))
      return "complete run provenance has invalid resolved configuration signed integer fields";
  }
  for (const auto key : config_unsigned_integer_fields) {
    if (!integer_lexeme_is(*config, key, true))
      return "complete run provenance has invalid resolved configuration unsigned integer fields";
  }
  const auto finite_number_is = [&field](const Object& object, std::string_view key) {
    const auto* value = field(object, std::string(key));
    if (value == nullptr || !value->is_number()) return false;
    const auto& lexeme = std::get<gemm_json::Number>(value->storage).lexeme;
    double parsed = 0.0;
    const auto [end, error] = std::from_chars(
        lexeme.data(), lexeme.data() + lexeme.size(), parsed, std::chars_format::general);
    return error == std::errc{} && end == lexeme.data() + lexeme.size() && std::isfinite(parsed);
  };
  for (const auto key : required_config_fields) {
    bool is_string_field = false;
    for (const auto string_key : config_string_fields) is_string_field = is_string_field || key == string_key;
    const auto* value = field(*config, std::string(key));
    if (!is_string_field && (value == nullptr || !value->is_number()))
      return "complete run provenance has invalid resolved configuration numeric fields";
    if (!is_string_field && !finite_number_is(*config, key))
      return "complete run provenance has non-finite or out-of-range resolved configuration numeric fields";
  }
  const auto* scenario = object_field(payload, "scenario");
  if (scenario == nullptr || string_field(*scenario, "name") != string_field(*config, "scenario_name") ||
      string_field(*scenario, "model_version") != string_field(*config, "model_version"))
    return "complete run provenance scenario does not match the resolved configuration";

  const auto config_string = [&string_field](const Object& object, std::string_view key) {
    return string_field(object, std::string(key));
  };
  const auto verify_runtime_input = [&runtime_files](std::string_view role,
                                                       const std::string& expected_path,
                                                       bool required) -> bool {
    const auto found = runtime_files.find(std::string(role));
    if (!required) return found == runtime_files.end();
    return found != runtime_files.end() && found->second.size() == 1 &&
           found->second.front().first == expected_path;
  };
  const auto& config_object = *config;
  const auto frame_mode = config_string(config_object, "frame_arrival_mode");
  if (frame_mode == "trace") {
    const auto trace_path = config_string(config_object, "frame_arrival_trace");
    const auto provenance_path = config_string(config_object, "frame_arrival_provenance");
    if (trace_path.empty() || provenance_path.empty() ||
        !verify_runtime_input("frame_arrival_trace", trace_path, true) ||
        !verify_runtime_input("frame_arrival_provenance", provenance_path, true))
      return "complete run provenance does not match configured frame-arrival inputs";
  } else if (!verify_runtime_input("frame_arrival_trace", "", false) ||
             !verify_runtime_input("frame_arrival_provenance", "", false)) {
    return "complete run provenance lists unused frame-arrival inputs";
  }
  if (config_string(config_object, "workload_mode") == "profiled_graph") {
    const auto profile_path = config_string(config_object, "workload_profile");
    const auto mapping_path = config_string(config_object, "mapping_plan");
    if (profile_path.empty() || !verify_runtime_input("workload_profile", profile_path, true) ||
        !verify_runtime_input("mapping_plan", mapping_path, !mapping_path.empty()))
      return "complete run provenance does not match configured profiled-workload inputs";
  } else if (!verify_runtime_input("workload_profile", "", false) ||
             !verify_runtime_input("mapping_plan", "", false)) {
    return "complete run provenance lists unused profiled-workload inputs";
  }

  const auto* overrides = object_field(payload, "explicit_cli_overrides");
  const auto* authored_values = object_field(payload, "authored_values_after_overrides");
  if (overrides == nullptr || authored_values == nullptr)
    return "complete run provenance omits explicit overrides or authored configuration values";
  for (const auto& [key, value] : *overrides) {
    if (!value.is_string()) return "complete run provenance has malformed CLI override values";
    const auto authored = authored_values->find(key);
    if (authored == authored_values->end() || !authored->second.is_string() ||
        authored->second.string() != value.string())
      return "complete run provenance CLI overrides do not match authored resolved values";
  }

  if (schema == "axissim.run-provenance.v2") {
    const auto* requirements = field(payload, "required_runtime_inputs");
    if (requirements == nullptr || !requirements->is_array())
      return "complete run provenance has no required runtime-input inventory";
    std::set<std::pair<std::string, std::string>> required_set;
    for (const auto& requirement : requirements->array()) {
      if (!requirement.is_object()) return "complete run provenance has a malformed runtime-input requirement";
      const auto& entry = requirement.object();
      const auto role = string_field(entry, "role");
      const auto path = string_field(entry, "path");
      const auto found = runtime_files.find(role);
      if (found == runtime_files.end() || found->second.size() != 1 || found->second.front().first != path ||
          !required_set.emplace(role, path).second)
        return "complete run provenance runtime-input requirements do not match its file inventory";
    }
    std::size_t runtime_count = 0;
    for (const auto& [role, records] : runtime_files) {
      if (records.size() != 1) return "complete run provenance has duplicate runtime-input roles";
      ++runtime_count;
      if (!required_set.contains({role, records.front().first}))
        return "complete run provenance omits a captured runtime input from its required-input inventory";
    }
    if (required_set.size() != runtime_count)
      return "complete run provenance required runtime-input inventory is inconsistent";
  }

  const auto* package = object_field(payload, "package");
  const auto package_kind = package == nullptr ? std::string{} : string_field(*package, "kind");
  const auto package_id = package == nullptr ? std::string{} : string_field(*package, "id");
  if (package == nullptr || (package_kind != "validation" && package_kind != "release") || !known(package_id))
    return "complete run provenance has incomplete package identity";
  if (package_kind == "release" &&
      (!known(string_field(*package, "release_tag")) || package_id != string_field(*package, "release_tag")))
    return "complete run provenance release identity is inconsistent";

  const auto* input_verification = object_field(payload, "input_verification");
  const auto expected_input_status = schema == "axissim.run-provenance.v1"
      ? "unchanged" : "exact_bytes_captured_and_used";
  if ((schema != "axissim.run-provenance.v1" && schema != "axissim.run-provenance.v2") ||
      input_verification == nullptr || string_field(*input_verification, "status") != expected_input_status)
    return "complete run provenance does not prove input stability";
  const auto* result = object_field(payload, "result");
  if (result == nullptr || string_field(*result, "status") != "hashed" ||
      !is_sha256(string_field(*result, "sha256")))
    return "complete run provenance has no hashed result";
  return std::nullopt;
}

std::string canonical_provenance_value(const Value& value) {
  return canonical_value(value);
}

std::string canonical_run_provenance(const ConfigLoadResult& loaded,
                                     const std::filesystem::path& root_manifest,
                                     const std::filesystem::path& executable,
                                     const std::vector<CapturedRuntimeInput>& captured_inputs) {
  return canonical_value(make_run_provenance(loaded, root_manifest, executable, captured_inputs));
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
