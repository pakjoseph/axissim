#include "gemm_json.hpp"
#include "gemm_study_contract.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
const std::string kRequest = R"json({
  "schema_id":"axissim.gemm-pair.request",
  "schema_version":1,
  "canonicalization_id":"axissim-json-canonical-v1",
  "workload":{"operation":"gemm","m":1,"n":1,"k":1,"batch_count":1,
    "input_format":"signed_i16_twos_complement","accumulator_output_format":"signed_i32_twos_complement",
    "overflow":"wrap_mod_2pow32","rounding":"none","layout":"row_major_contiguous",
    "transpose_a":false,"transpose_b":false,"initial_c":"zero","operand_data_identity":"shape_only_no_values_v1"},
  "measurement":{"protocol":"finite_serial_batch_v1","repeat_count":2,"release_policy":"all_at_zero",
    "unit_order":"ascending_index_no_overlap","clock_hz":1000000000},
  "mappings":{
    "rvv":{"kind":"rvv_row_v1","vlen_bits":128,"sew_bits":16,"lmul":1,"lanes":4,
      "macs_per_lane_cycle":1,"accumulator_recurrence_cycles":1,"kernel_setup_cycles":0,"chunk_setup_cycles":0},
    "matrix":{"kind":"tiled_matrix_output_stationary_v1","tile_m":1,"tile_n":1,"tile_k":1,
      "local_capacity_bytes":8,"mac_slots_per_cycle":4,"accumulator_recurrence_cycles":1,
      "kernel_setup_cycles":0,"tile_setup_cycles":0}}
})json";
const axissim::gemm_json::Value &get(const axissim::gemm_json::Value &object,
                                     const char *key) {
  return object.object().at(key);
}
std::uint64_t number(const axissim::gemm_json::Value &value) {
  return value.unsigned_integer();
}
std::string string(const axissim::gemm_json::Value &value) {
  return value.string();
}
std::string replace_once(std::string source, const std::string &from,
                         const std::string &to) {
  const auto position = source.find(from);
  require(position != std::string::npos,
          "test fixture replacement target missing");
  source.replace(position, from.size(), to);
  return source;
}
void exact_fields(const axissim::gemm_json::Value &value,
                  std::initializer_list<const char *> keys,
                  const char *message) {
  std::set<std::string> expected;
  for (const auto *key : keys)
    expected.emplace(key);
  std::set<std::string> actual;
  for (const auto &[key, unused] : value.object()) {
    (void)unused;
    actual.emplace(key);
  }
  require(actual == expected, message);
}
} // namespace

int main() {
  using namespace axissim;
  const auto first = evaluate_study_gemm_pair(kRequest);
  const auto second = evaluate_study_gemm_pair(kRequest);
  require(first.exit_code == 0, "valid pair request did not complete");
  require(first.json == second.json,
          "identical request did not produce byte-identical output");
  const auto result = gemm_json::parse(first.json);
  require(string(get(result, "schema_id")) == "axissim.gemm-pair.result",
          "wrong result schema");
  require(string(get(result, "status")) == "complete",
          "valid pair result is not complete");
  require(result.object().size() == 19,
          "unexpected top-level result field count");
  exact_fields(result,
               {"schema_id", "schema_version", "canonicalization_id",
                "candidate_pair_id", "request_sha256",
                "model_assumptions_sha256", "simulator", "workload_id",
                "measurement_protocol", "repeat_count", "status", "arms",
                "model_coverage", "errors", "energy_status", "power_status",
                "budget_status", "physical_feasibility", "uncertainty_class"},
               "top-level result fields differ from v1");
  const auto &arms = get(result, "arms");
  const auto &rvv = get(arms, "rvv");
  const auto &matrix = get(arms, "matrix");
  exact_fields(rvv,
               {"arm_status", "mapping_id", "mapping_kind", "workload_counts",
                "vector_mac_instructions_per_unit",
                "vector_mac_instructions_total", "logical_traffic",
                "compute_cycles_per_unit", "compute_cycles_total",
                "compute_duration_seconds", "finite_compute_makespan_seconds",
                "finite_batch_rate", "p99_compute_completion",
                "modeled_mac_slot_utilization"},
               "RVV arm fields differ from v1");
  exact_fields(
      matrix,
      {"arm_status", "mapping_id", "mapping_kind", "workload_counts",
       "matrix_tile_operations_per_unit", "matrix_tile_operations_total",
       "logical_traffic", "compute_cycles_per_unit", "compute_cycles_total",
       "compute_duration_seconds", "finite_compute_makespan_seconds",
       "finite_batch_rate", "p99_compute_completion",
       "modeled_mac_slot_utilization", "working_set_bytes", "capacity_fit"},
      "matrix arm fields differ from v1");
  require(number(get(get(rvv, "workload_counts"), "useful_macs_per_unit")) == 1,
          "wrong RVV useful MAC count");
  require(number(get(get(rvv, "workload_counts"),
                     "conventional_ops_per_unit")) == 2,
          "conventional operations must count two operations per MAC");
  require(number(get(rvv, "compute_cycles_per_unit")) == 1,
          "wrong RVV cycle estimate");
  require(number(get(matrix, "working_set_bytes")) == 8,
          "wrong matrix working-set size");
  require(number(get(matrix, "compute_cycles_total")) == 2,
          "wrong finite matrix makespan");
  require(number(get(get(matrix, "p99_compute_completion"), "p99_rank")) == 2,
          "wrong nearest-rank P99");
  require(string(get(result, "energy_status")) == "not_modeled",
          "energy must remain unmodeled");

  const auto changed_mapping = evaluate_study_gemm_pair(
      replace_once(kRequest, "\"tile_m\":1", "\"tile_m\":2"));
  const auto changed = gemm_json::parse(changed_mapping.json);
  require(string(get(result, "candidate_pair_id")) !=
              string(get(changed, "candidate_pair_id")),
          "mapping change did not change candidate identity");

  const auto capacity = evaluate_study_gemm_pair(replace_once(
      kRequest, "\"local_capacity_bytes\":8", "\"local_capacity_bytes\":7"));
  const auto capacity_result = gemm_json::parse(capacity.json);
  require(capacity.exit_code == 3 &&
              string(get(capacity_result, "status")) == "partial",
          "matrix capacity failure did not produce a partial result");
  require(string(get(get(get(capacity_result, "arms"), "matrix"),
                     "arm_status")) == "invalid",
          "matrix capacity failure was not scoped to that arm");
  require(!get(get(capacity_result, "arms"), "matrix")
               .object()
               .contains("compute_cycles_total"),
          "invalid arm should omit metrics");

  auto bad_json = evaluate_study_gemm_pair("{");
  require(bad_json.exit_code == 2 &&
              bad_json.json ==
                  "{\"errors\":[{\"code\":\"invalid_json\",\"path\":\"\","
                  "\"scope\":\"request\"}],\"schema_id\":\"axissim.gemm-pair."
                  "error\",\"schema_version\":1,\"status\":\"invalid_json\"}",
          "invalid JSON envelope is not canonical or exact");
  const auto unknown = evaluate_study_gemm_pair(replace_once(
      kRequest, "\"schema_version\":1", "\"schema_version\":1,\"surprise\":0"));
  const auto unknown_result = gemm_json::parse(unknown.json);
  require(string(get(unknown_result, "status")) == "invalid_request",
          "unknown field status mismatch");
  require(string(get(get(unknown_result, "errors").array()[0], "code")) ==
              "unknown_field",
          "unknown field was not reported explicitly");

  const auto unsupported = evaluate_study_gemm_pair(
      replace_once(kRequest, "\"schema_version\":1", "\"schema_version\":2"));
  require(string(get(gemm_json::parse(unsupported.json), "status")) ==
              "unsupported_schema",
          "unsupported schema version was accepted");

  const auto sensitive_request = replace_once(
      kRequest, "\"clock_hz\":1000000000",
      "\"clock_hz\":1000000000,\"backing_store_sensitivity\":{\"bandwidth_"
      "bytes_per_second\":1000000000,\"batch_setup_cycles\":0}");
  const auto sensitive =
      gemm_json::parse(evaluate_study_gemm_pair(sensitive_request).json);
  const auto &sensitive_rvv = get(get(sensitive, "arms"), "rvv");
  require(get(sensitive_rvv, "serialized_backing_store").is_object(),
          "requested serialized sensitivity was omitted");
  require(string(get(get(sensitive, "model_coverage"), "memory_service")) ==
              "serialized_sensitivity_only",
          "serialized sensitivity coverage was not labeled");
  require(number(get(sensitive_rvv, "compute_cycles_per_unit")) == 1,
          "serialized sensitivity changed the primary compute timing");
  require(number(get(get(sensitive_rvv, "serialized_backing_store"),
                     "finite_makespan_cycles")) > 2,
          "serialized sensitivity did not include transfers");

  const auto fractional =
      evaluate_study_gemm_pair(replace_once(kRequest, "\"m\":1", "\"m\":1.0"));
  const auto fractional_result = gemm_json::parse(fractional.json);
  require(string(get(fractional_result, "status")) == "invalid_request",
          "fractional JSON number was accepted for a dimension");
  require(string(get(get(fractional_result, "errors").array()[0], "code")) ==
              "invalid_type",
          "fractional dimension had the wrong error class");
  const auto unsafe_integer = evaluate_study_gemm_pair(
      replace_once(kRequest, "\"m\":1", "\"m\":9007199254740992"));
  const auto unsafe_result = gemm_json::parse(unsafe_integer.json);
  require(string(get(get(unsafe_result, "errors").array()[0], "code")) ==
                  "invalid_value" &&
              string(get(get(unsafe_result, "errors").array()[0], "path")) ==
                  "/workload/m",
          "integer beyond the JSON-safe limit was not rejected precisely");
  const auto non_ascii_request = evaluate_study_gemm_pair(replace_once(
      kRequest, "\"operation\":\"gemm\"", "\"operation\":\"g\\u00e9mm\""));
  const auto non_ascii_result = gemm_json::parse(non_ascii_request.json);
  require(string(get(non_ascii_result, "status")) == "invalid_request" &&
              string(get(get(non_ascii_result, "errors").array()[0], "path")) ==
                  "/workload/operation",
          "escaped non-ASCII request text was not rejected at its field");

  const auto ordered = evaluate_study_gemm_pair(
      replace_once(replace_once(kRequest, "\"m\":1", "\"m\":0"),
                   "\"operation\":\"gemm\"", "\"operation\":\"gemv\""));
  const auto ordered_result = gemm_json::parse(ordered.json);
  require(string(get(get(ordered_result, "errors").array()[0], "path")) ==
              "/workload/m",
          "request validation errors were not selected by lexicographic JSON "
          "pointer");

  const auto duplicate =
      evaluate_study_gemm_pair("{\"schema_id\":\"axissim.gemm-pair.request\","
                               "\"schema_id\":\"axissim.gemm-pair.request\"}");
  const auto duplicate_result = gemm_json::parse(duplicate.json);
  require(string(get(duplicate_result, "status")) == "invalid_json" &&
              string(get(get(duplicate_result, "errors").array()[0], "code")) ==
                  "duplicate_key",
          "duplicate request key was not rejected");

  const auto input_cap =
      evaluate_study_gemm_pair(std::string(64 * 1024 + 1, ' '));
  const auto cap_result = gemm_json::parse(input_cap.json);
  require(input_cap.exit_code == 2 &&
              string(get(cap_result, "status")) == "input_limit" &&
              string(get(get(cap_result, "errors").array()[0], "code")) ==
                  "request_too_large",
          "request input cap was not enforced");
  return 0;
}
