#include "profile.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace axissim {
namespace {

[[nodiscard]] std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

}  // namespace


[[nodiscard]] std::optional<std::string> json_string_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) return std::nullopt;
  return match[1].str();
}

[[nodiscard]] std::optional<std::uint64_t> json_unsigned_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) return std::nullopt;
  try {
    return static_cast<std::uint64_t>(std::stoull(match[1].str()));
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid unsigned JSON field: " + key);
  }
}

[[nodiscard]] std::optional<double> json_double_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*([-+]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][-+]?[0-9]+)?)");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) return std::nullopt;
  try {
    const double value = std::stod(match[1].str());
    if (!std::isfinite(value)) throw std::runtime_error("invalid");
    return value;
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid numeric JSON field: " + key);
  }
}

[[nodiscard]] std::vector<std::string> json_string_array_field(const std::string& object, const std::string& key) {
  const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*\\[([^\\]]*)\\]");
  std::smatch match;
  if (!std::regex_search(object, match, expression)) throw std::runtime_error("Profile operator missing array field: " + key);
  const std::string contents = trim(match[1].str());
  if (contents.empty()) return {};
  std::vector<std::string> values;
  const std::regex item("\\\"([^\\\"]+)\\\"");
  for (std::sregex_iterator iterator(contents.begin(), contents.end(), item), end; iterator != end; ++iterator) values.push_back((*iterator)[1].str());
  if (values.empty()) throw std::runtime_error("Profile array field has invalid JSON strings: " + key);
  return values;
}

[[nodiscard]] std::vector<std::string> json_array_objects(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  const auto key_position = json.find(marker);
  if (key_position == std::string::npos) throw std::runtime_error("Profile is missing array: " + key);
  const auto array_start = json.find('[', key_position + marker.size());
  if (array_start == std::string::npos) throw std::runtime_error("Profile array is malformed: " + key);
  std::vector<std::string> objects;
  int array_depth = 0;
  int object_depth = 0;
  std::size_t object_start = std::string::npos;
  for (std::size_t index = array_start; index < json.size(); ++index) {
    const char character = json[index];
    if (character == '[') ++array_depth;
    if (character == ']') {
      --array_depth;
      if (array_depth == 0) break;
    }
    if (array_depth <= 0) continue;
    if (character == '{') {
      if (object_depth++ == 0) object_start = index;
    } else if (character == '}') {
      if (--object_depth < 0) throw std::runtime_error("Profile object nesting is malformed");
      if (object_depth == 0 && object_start != std::string::npos) objects.push_back(json.substr(object_start, index - object_start + 1));
    }
  }
  if (array_depth != 0 || object_depth != 0 || objects.empty()) throw std::runtime_error("Profile operators array is malformed or empty");
  return objects;
}

WorkloadProfile load_workload_profile_text(const std::string& json) {
  if (json_string_field(json, "schema_version") != std::optional<std::string>("axissim.workload-profile.v1")) {
    throw std::runtime_error("Unsupported workload-profile schema; expected axissim.workload-profile.v1");
  }
  WorkloadProfile profile;
  profile.source_url = json_string_field(json, "source_url").value_or("");
  profile.license = json_string_field(json, "license").value_or("");
  profile.sha256 = json_string_field(json, "model_sha256").value_or("");
  profile.declared_storage_precision = json_string_field(json, "tensor_storage_precision").value_or("");
  if (profile.source_url.empty() || profile.license.empty() || profile.sha256.size() != 64) {
    throw std::runtime_error("Workload profile source URL, license, and 64-character SHA-256 are required");
  }
  const auto objects = json_array_objects(json, "operators");
  std::set<std::string> ids;
  for (const auto& object : objects) {
    ProfileOperator operation;
    operation.id = json_string_field(object, "id").value_or("");
    operation.mapping_class = json_string_field(object, "mapping_class").value_or("");
    operation.dependencies = json_string_array_field(object, "dependencies");
    operation.inputs = json_string_array_field(object, "inputs");
    operation.outputs = json_string_array_field(object, "outputs");
    operation.macs = json_unsigned_field(object, "macs").value_or(0);
    operation.vector_elements = json_unsigned_field(object, "vector_elements").value_or(0);
    operation.ops_per_element = static_cast<int>(json_unsigned_field(object, "ops_per_element").value_or(0));
    operation.activation_read_bytes = json_unsigned_field(object, "activation_read_bytes").value_or(0);
    operation.activation_write_bytes = json_unsigned_field(object, "activation_write_bytes").value_or(0);
    operation.weight_bytes = json_unsigned_field(object, "weight_bytes").value_or(0);
    operation.transfer_bytes = operation.activation_read_bytes + operation.activation_write_bytes + operation.weight_bytes;
    if (operation.id.empty() || !ids.insert(operation.id).second) throw std::runtime_error("Profile operator IDs must be non-empty and unique");
    if (operation.mapping_class == "npu_compute" && operation.macs == 0) throw std::runtime_error("NPU profile operator requires positive MAC count: " + operation.id);
    if (operation.mapping_class == "rvv_elementwise" && (operation.vector_elements == 0 || operation.ops_per_element <= 0)) throw std::runtime_error("RVV profile operator requires positive vector geometry: " + operation.id);
    if (operation.mapping_class != "npu_compute" && operation.mapping_class != "rvv_elementwise") throw std::runtime_error("Unsupported profile mapping class: " + operation.mapping_class);
    profile.operators.push_back(std::move(operation));
  }
  std::map<std::string, int> indegree;
  std::map<std::string, std::vector<std::string>> successors;
  std::map<std::string, std::string> producer;
  for (const auto& operation : profile.operators) indegree.emplace(operation.id, 0);
  for (const auto& operation : profile.operators) {
    if (operation.outputs.empty()) throw std::runtime_error("Profile operator must name at least one output: " + operation.id);
    for (const auto& output : operation.outputs) {
      if (output.empty() || !producer.emplace(output, operation.id).second) throw std::runtime_error("Profile tensor outputs must have one non-empty producer: " + output);
    }
  }
  for (const auto& operation : profile.operators) {
    for (const auto& dependency : operation.dependencies) {
      if (!ids.contains(dependency)) throw std::runtime_error("Profile dependency does not name an operator: " + dependency);
      ++indegree.at(operation.id);
      successors[dependency].push_back(operation.id);
    }
    for (const auto& input_name : operation.inputs) {
      if (const auto producer_it = producer.find(input_name); producer_it != producer.end() &&
          std::find(operation.dependencies.cbegin(), operation.dependencies.cend(), producer_it->second) == operation.dependencies.cend()) {
        throw std::runtime_error("Profile dependency list omits producer " + producer_it->second + " for input tensor " + input_name);
      }
    }
  }
  std::queue<std::string> ready;
  for (const auto& [id, count] : indegree) if (count == 0) ready.push(id);
  int visited = 0;
  while (!ready.empty()) {
    const auto id = ready.front(); ready.pop(); ++visited;
    for (const auto& successor : successors[id]) if (--indegree.at(successor) == 0) ready.push(successor);
  }
  if (visited != static_cast<int>(profile.operators.size())) throw std::runtime_error("Workload profile dependency graph contains a cycle");
  return profile;
}

WorkloadProfile load_workload_profile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to open workload profile: " + path);
  const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return load_workload_profile_text(json);
}

MappingPlan load_mapping_plan_text(const std::string& path, const WorkloadProfile& profile, const std::string& json) {
  if (json_string_field(json, "schema_version") != std::optional<std::string>("axissim.mapping-plan.v1")) {
    throw std::runtime_error("Unsupported mapping-plan schema; expected axissim.mapping-plan.v1");
  }
  MappingPlan plan;
  plan.id = json_string_field(json, "id").value_or("");
  plan.source_model_sha256 = json_string_field(json, "source_model_sha256").value_or("");
  plan.mapping_rule_version = json_string_field(json, "mapping_rule_version").value_or("");
  if (plan.id.empty() || plan.mapping_rule_version.empty() || plan.source_model_sha256 != profile.sha256) {
    throw std::runtime_error("Mapping plan requires id/rule version and an exact workload source-model SHA-256 match");
  }
  const bool has_source_precision = json.find("\"source_storage_precision\"") != std::string::npos;
  const bool has_target_precision = json.find("\"target_storage_precision\"") != std::string::npos;
  if (has_source_precision != has_target_precision) {
    throw std::runtime_error("Mapping-plan precision policy must name both source_storage_precision and target_storage_precision");
  }
  if (has_source_precision) {
    plan.source_storage_precision = json_string_field(json, "source_storage_precision").value_or("");
    plan.target_storage_precision = json_string_field(json, "target_storage_precision").value_or("");
    const auto valid_precision = [](const std::string& value) { return value == "fp32" || value == "fp16" || value == "int8"; };
    if (!valid_precision(plan.source_storage_precision) || !valid_precision(plan.target_storage_precision)) {
      throw std::runtime_error("Mapping-plan storage precision must be fp32, fp16, or int8");
    }
    if (profile.declared_storage_precision != plan.source_storage_precision) {
      throw std::runtime_error("Mapping-plan source storage precision must match the workload profile's declared tensor_storage_precision");
    }
  }
  plan.npu_arithmetic_energy_scale = json_double_field(json, "npu_arithmetic_energy_scale").value_or(1.0);
  if (plan.npu_arithmetic_energy_scale <= 0.0) {
    throw std::runtime_error("Mapping-plan npu_arithmetic_energy_scale must be positive");
  }
  if (const auto evidence = json_string_field(json, "accuracy_evidence")) {
    plan.accuracy_evidence_path = (std::filesystem::path(path).parent_path() / *evidence).lexically_normal().string();
    std::ifstream evidence_input(plan.accuracy_evidence_path);
    if (!evidence_input) throw std::runtime_error("Unable to open mapping-plan accuracy evidence: " + plan.accuracy_evidence_path);
    const std::string evidence_json((std::istreambuf_iterator<char>(evidence_input)), std::istreambuf_iterator<char>());
    if (json_string_field(evidence_json, "schema_version") != std::optional<std::string>("axissim.accuracy-evidence.v1") ||
        json_string_field(evidence_json, "mapping_plan_id") != std::optional<std::string>(plan.id) ||
        json_string_field(evidence_json, "source_model_sha256") != std::optional<std::string>(profile.sha256) ||
        json_string_field(evidence_json, "evaluation_status") != std::optional<std::string>("external_evaluated")) {
      throw std::runtime_error("Accuracy evidence must bind this mapping plan, source-model SHA-256, and external_evaluated status");
    }
    const auto metric = json_string_field(evidence_json, "metric").value_or("");
    const auto dataset = json_string_field(evidence_json, "dataset").value_or("");
    const auto evaluator = json_string_field(evidence_json, "evaluator").value_or("");
    const auto source_url = json_string_field(evidence_json, "source_url").value_or("");
    const auto value = json_double_field(evidence_json, "value");
    if (metric.empty() || dataset.empty() || evaluator.empty() || source_url.empty() || !value) throw std::runtime_error("Accuracy evidence requires metric, value, dataset, evaluator, and source_url");
  }
  const auto objects = json_array_objects(json, "entries");
  std::set<std::string> mapped_ids;
  for (const auto& object : objects) {
    MappingPlanEntry entry;
    entry.operator_id = json_string_field(object, "operator_id").value_or("");
    entry.mapping_class = json_string_field(object, "mapping_class").value_or("");
    if (entry.operator_id.empty() || !mapped_ids.insert(entry.operator_id).second) {
      throw std::runtime_error("Mapping plan operator IDs must be non-empty and unique");
    }
    if (entry.mapping_class != "npu_compute" && entry.mapping_class != "rvv_elementwise") {
      throw std::runtime_error("Unsupported mapping-plan placement class: " + entry.mapping_class);
    }
    entry.weight_residency = json_string_field(object, "weight_residency").value_or("streamed");
    if (entry.weight_residency != "streamed" && entry.weight_residency != "persistent") {
      throw std::runtime_error("Unsupported mapping-plan weight residency: " + entry.weight_residency);
    }
    plan.entries.push_back(std::move(entry));
  }
  if (plan.entries.size() != profile.operators.size()) {
    throw std::runtime_error("Mapping plan must account for every workload-profile operator exactly once");
  }
  if (json.find("\"fusions\"") != std::string::npos) {
    std::set<std::string> participating_operators;
    const auto fusion_objects = json_array_objects(json, "fusions");
    std::map<std::string, const ProfileOperator*> operators_by_id;
    for (const auto& operation : profile.operators) operators_by_id.emplace(operation.id, &operation);
    for (const auto& object : fusion_objects) {
      MappingFusion fusion;
      fusion.producer_id = json_string_field(object, "producer_id").value_or("");
      fusion.consumer_id = json_string_field(object, "consumer_id").value_or("");
      fusion.tensor = json_string_field(object, "tensor").value_or("");
      const auto producer = operators_by_id.find(fusion.producer_id);
      const auto consumer = operators_by_id.find(fusion.consumer_id);
      if (fusion.producer_id.empty() || fusion.consumer_id.empty() || fusion.tensor.empty() || producer == operators_by_id.end() || consumer == operators_by_id.end()) {
        throw std::runtime_error("Fusion must name existing producer, consumer, and tensor IDs");
      }
      if (!participating_operators.insert(fusion.producer_id).second || !participating_operators.insert(fusion.consumer_id).second) {
        throw std::runtime_error("V1 fusion plans cannot overlap operators");
      }
      const auto& producer_op = *producer->second;
      const auto& consumer_op = *consumer->second;
      if (producer_op.outputs.size() != 1 || producer_op.outputs.front() != fusion.tensor ||
          std::find(consumer_op.inputs.cbegin(), consumer_op.inputs.cend(), fusion.tensor) == consumer_op.inputs.cend() ||
          std::find(consumer_op.dependencies.cbegin(), consumer_op.dependencies.cend(), fusion.producer_id) == consumer_op.dependencies.cend()) {
        throw std::runtime_error("Fusion must be a direct declared producer-consumer tensor edge");
      }
      const int consumers = static_cast<int>(std::count_if(profile.operators.cbegin(), profile.operators.cend(), [&fusion](const ProfileOperator& operation) {
        return std::find(operation.inputs.cbegin(), operation.inputs.cend(), fusion.tensor) != operation.inputs.cend();
      }));
      if (consumers != 1) throw std::runtime_error("V1 fusion requires a single-consumer intermediate tensor");
      if (producer_op.activation_write_bytes == 0 || producer_op.activation_write_bytes > consumer_op.activation_read_bytes) {
        throw std::runtime_error("Fusion tensor bytes cannot be reconciled with declared activation traffic");
      }
      plan.fusions.push_back(std::move(fusion));
    }
  }
  return plan;
}

MappingPlan load_mapping_plan(const std::string& path, const WorkloadProfile& profile) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to open mapping plan: " + path);
  const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return load_mapping_plan_text(path,profile,json);
}

void apply_mapping_plan(WorkloadProfile& profile, const MappingPlan& plan, const Config& config) {
  std::map<std::string, MappingPlanEntry> mapping_by_id;
  for (const auto& entry : plan.entries) mapping_by_id.emplace(entry.operator_id, entry);
  const auto precision_bits = [](const std::string& value) -> std::uint64_t {
    if (value == "fp32") return 32;
    if (value == "fp16") return 16;
    if (value == "int8") return 8;
    throw std::runtime_error("Unsupported storage precision");
  };
  const bool precision_changed = !plan.source_storage_precision.empty() && plan.source_storage_precision != plan.target_storage_precision;
  const auto source_bits = precision_changed ? precision_bits(plan.source_storage_precision) : std::uint64_t{0};
  const auto target_bits = precision_changed ? precision_bits(plan.target_storage_precision) : std::uint64_t{0};
  const auto scale_bytes = [source_bits, target_bits](std::uint64_t bytes, const std::string& operator_id) {
    if (bytes == 0) return bytes;
    if (source_bits < target_bits || bytes > std::numeric_limits<std::uint64_t>::max() / target_bits ||
        (bytes * target_bits) % source_bits != 0) {
      throw std::runtime_error("Mapping-plan precision cannot exactly scale profiled bytes for operator " + operator_id);
    }
    return bytes * target_bits / source_bits;
  };
  std::uint64_t persistent_weight_bytes = 0;
  for (auto& operation : profile.operators) {
    const auto it = mapping_by_id.find(operation.id);
    if (it == mapping_by_id.end()) throw std::runtime_error("Mapping plan omits workload operator: " + operation.id);
    // V1 makes the current implicit placement explicit. Cross-engine
    // placement becomes legal only once each profile records a valid cost
    // model for both resources; never invent a zero-cost alternate mapping.
    if (it->second.mapping_class != operation.mapping_class) {
      throw std::runtime_error("Mapping-plan placement is unsupported for operator " + operation.id + " in v1; provide the baseline class");
    }
    if (precision_changed) {
      const auto before = operation.transfer_bytes;
      operation.activation_read_bytes = scale_bytes(operation.activation_read_bytes, operation.id);
      operation.activation_write_bytes = scale_bytes(operation.activation_write_bytes, operation.id);
      operation.weight_bytes = scale_bytes(operation.weight_bytes, operation.id);
      operation.transfer_bytes = operation.activation_read_bytes + operation.activation_write_bytes + operation.weight_bytes;
      profile.precision_traffic_bytes_avoided_per_frame += before - operation.transfer_bytes;
    }
    if (operation.mapping_class == "npu_compute") operation.npu_arithmetic_energy_scale = plan.npu_arithmetic_energy_scale;
    if (it->second.weight_residency == "persistent") {
      if (operation.mapping_class != "npu_compute" || operation.weight_bytes == 0) {
        throw std::runtime_error("Persistent weight residency requires an NPU operator with declared weights: " + operation.id);
      }
      operation.persistent_weights = true;
      persistent_weight_bytes += operation.weight_bytes;
      profile.persistent_weight_bytes_avoided_per_steady_frame += operation.weight_bytes;
      ++profile.persistent_weight_operator_count;
    }
  }
  if (persistent_weight_bytes > config.weight_sram_capacity_bytes) {
    throw std::runtime_error("Mapping-plan persistent weights require " + std::to_string(persistent_weight_bytes) +
                             " bytes of weight SRAM; configured capacity is " + std::to_string(config.weight_sram_capacity_bytes));
  }
  std::map<std::string, ProfileOperator*> operators_by_id;
  for (auto& operation : profile.operators) operators_by_id.emplace(operation.id, &operation);
  for (const auto& fusion : plan.fusions) {
    auto& producer = *operators_by_id.at(fusion.producer_id);
    auto& consumer = *operators_by_id.at(fusion.consumer_id);
    const auto bytes = producer.activation_write_bytes;
    producer.activation_write_bytes -= bytes;
    consumer.activation_read_bytes -= bytes;
    producer.transfer_bytes = producer.activation_read_bytes + producer.activation_write_bytes + producer.weight_bytes;
    consumer.transfer_bytes = consumer.activation_read_bytes + consumer.activation_write_bytes + consumer.weight_bytes;
    profile.fused_intermediate_bytes_avoided += 2 * bytes;
    ++profile.fusion_count;
  }
  profile.mapping_plan_id = plan.id;
  profile.mapping_rule_version = plan.mapping_rule_version;
  if (!plan.source_storage_precision.empty()) {
    profile.source_storage_precision = plan.source_storage_precision;
    profile.target_storage_precision = plan.target_storage_precision;
  }
  profile.mapped_npu_arithmetic_energy_scale = plan.npu_arithmetic_energy_scale;
  if (!plan.accuracy_evidence_path.empty()) {
    std::ifstream input(plan.accuracy_evidence_path);
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    profile.accuracy_evidence_status = "external_evaluated";
    profile.accuracy_metric = json_string_field(json, "metric").value_or("");
    profile.accuracy_dataset = json_string_field(json, "dataset").value_or("");
    profile.accuracy_evaluator = json_string_field(json, "evaluator").value_or("");
    profile.accuracy_source_url = json_string_field(json, "source_url").value_or("");
    profile.accuracy_value = *json_double_field(json, "value");
  }
}

}  // namespace axissim

