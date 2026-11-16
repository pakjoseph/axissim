#include "gemm_study_contract.hpp"

#include "gemm_json.hpp"
#include "gemm_mapping.hpp"
#include "gemm_workload.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace axissim {
namespace {
using gemm_json::Array;
using gemm_json::Object;
using gemm_json::Value;

constexpr std::uint64_t kMaxSafeInteger = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kMaxUsefulMacs = 1ULL << 32;
constexpr std::uint64_t kMaxMappingOperations = 1'000'000;
constexpr std::uint64_t kMaxLogicalBytes = 1'000'000'000;
constexpr std::uint64_t kMaxCycles =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::size_t kMaxInputBytes = 64 * 1024;
constexpr std::size_t kMaxOutputBytes = 64 * 1024;
constexpr std::string_view kSchema = "axissim.gemm-pair.request";
constexpr std::string_view kCanonicalization = "axissim-json-canonical-v1";

struct RequestError {
  std::string code;
  std::string path;
};

struct Request {
  Value tree;
  Value workload_tree;
  Value measurement_tree;
  Value rvv_tree;
  Value matrix_tree;
  GemmWorkload workload;
  RvvGemmMapping rvv;
  TiledMatrixGemmMapping matrix;
  std::uint64_t repeat_count = 0;
  std::uint64_t clock_hz = 0;
  std::optional<SerializedBackingStoreParameters> backing_store;
  std::string request_sha;
  std::string workload_id;
  std::string rvv_id;
  std::string matrix_id;
  std::string pair_id;
  std::string assumptions_sha;
};

void preflight_limits(const Request &request);

[[noreturn]] void bad(std::string code, std::string path) {
  throw RequestError{std::move(code), std::move(path)};
}

[[nodiscard]] Value object_value(Object object) {
  return Value(std::move(object));
}
[[nodiscard]] Value uint_value(std::uint64_t value) { return Value(value); }
[[nodiscard]] Value string_value(std::string value) {
  return Value(std::move(value));
}

[[nodiscard]] std::string hex_digest(std::string_view bytes) {
  return sha256_hex(bytes);
}

[[nodiscard]] bool unsigned_integer_lexeme(std::string_view lexeme) {
  return !lexeme.empty() &&
         std::all_of(lexeme.begin(), lexeme.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

[[nodiscard]] bool exceeds_safe_integer(std::string_view lexeme) {
  constexpr std::string_view max_safe = "9007199254740991";
  return lexeme.size() > max_safe.size() ||
         (lexeme.size() == max_safe.size() && lexeme > max_safe);
}

[[nodiscard]] std::string path_child(std::string_view parent,
                                     std::string_view key) {
  return gemm_json::pointer_child(parent, key);
}

void validate_ascii_and_canonical_types(const Value &value,
                                        const std::string &path) {
  if (value.is_null())
    bad("invalid_value", path);
  if (value.is_string()) {
    for (const unsigned char c : value.string())
      if (c > 0x7f)
        bad("invalid_value", path);
    return;
  }
  if (value.is_number()) {
    const auto &lexeme = std::get<gemm_json::Number>(value.storage).lexeme;
    if (!unsigned_integer_lexeme(lexeme))
      bad("invalid_type", path);
    if (exceeds_safe_integer(lexeme))
      bad("invalid_value", path);
    return;
  }
  if (value.is_array()) {
    for (std::size_t i = 0; i < value.array().size(); ++i)
      validate_ascii_and_canonical_types(value.array()[i],
                                         path_child(path, std::to_string(i)));
    return;
  }
  if (value.is_object()) {
    for (const auto &[key, child] : value.object()) {
      for (const unsigned char c : key)
        if (c > 0x7f)
          bad("invalid_value", path_child(path, key));
      validate_ascii_and_canonical_types(child, path_child(path, key));
    }
  }
}

[[nodiscard]] const Object &as_object(const Value &value,
                                      const std::string &path) {
  if (!value.is_object())
    bad("invalid_type", path);
  return value.object();
}

void exact_keys(const Object &object,
                std::initializer_list<std::string_view> required,
                const std::string &path,
                std::initializer_list<std::string_view> optional = {}) {
  std::vector<std::string> allowed;
  allowed.reserve(required.size());
  for (const auto key : required)
    allowed.emplace_back(key);
  for (const auto key : optional)
    allowed.emplace_back(key);
  std::sort(allowed.begin(), allowed.end());
  for (const auto &[key, unused] : object) {
    (void)unused;
    if (!std::binary_search(allowed.begin(), allowed.end(), key))
      bad("unknown_field", path_child(path, key));
  }
  for (const auto key : required) {
    const std::string name(key);
    if (!object.contains(name))
      bad("invalid_value", path_child(path, name));
  }
}

[[nodiscard]] const Value &field(const Object &object, std::string_view key,
                                 const std::string &path) {
  const auto found = object.find(std::string(key));
  if (found == object.end())
    bad("invalid_value", path_child(path, key));
  return found->second;
}

[[nodiscard]] std::uint64_t unsigned_field(const Object &object,
                                           std::string_view key,
                                           const std::string &path,
                                           bool positive) {
  const auto &value = field(object, key, path);
  const auto value_path = path_child(path, key);
  if (!value.is_number())
    bad("invalid_type", value_path);
  const auto &lexeme = std::get<gemm_json::Number>(value.storage).lexeme;
  if (!unsigned_integer_lexeme(lexeme))
    bad("invalid_type", value_path);
  if (exceeds_safe_integer(lexeme))
    bad("invalid_value", value_path);
  const auto number = value.unsigned_integer();
  if (number > kMaxSafeInteger || (positive && number == 0))
    bad("invalid_value", value_path);
  return number;
}

[[nodiscard]] std::string string_field(const Object &object,
                                       std::string_view key,
                                       const std::string &path) {
  const auto &value = field(object, key, path);
  if (!value.is_string())
    bad("invalid_type", path_child(path, key));
  return value.string();
}

void enum_field(const Object &object, std::string_view key,
                const std::string &path, std::string_view expected) {
  if (string_field(object, key, path) != expected)
    bad("invalid_value", path_child(path, key));
}

void bool_field(const Object &object, std::string_view key,
                const std::string &path, bool expected) {
  const auto &value = field(object, key, path);
  if (!value.is_bool())
    bad("invalid_type", path_child(path, key));
  if (std::get<bool>(value.storage) != expected)
    bad("invalid_value", path_child(path, key));
}

void note(std::vector<RequestError> &errors, std::string code,
          std::string path) {
  errors.push_back({std::move(code), std::move(path)});
}

const Object *collect_object(const Value &value, std::string path,
                             std::initializer_list<std::string_view> required,
                             std::initializer_list<std::string_view> optional,
                             std::vector<RequestError> &errors) {
  if (!value.is_object()) {
    note(errors, "invalid_type", std::move(path));
    return nullptr;
  }
  const auto &object = value.object();
  std::vector<std::string> allowed;
  for (const auto key : required)
    allowed.emplace_back(key);
  for (const auto key : optional)
    allowed.emplace_back(key);
  std::sort(allowed.begin(), allowed.end());
  for (const auto &[key, unused] : object) {
    (void)unused;
    if (!std::binary_search(allowed.begin(), allowed.end(), key))
      note(errors, "unknown_field", path_child(path, key));
  }
  for (const auto key : required) {
    const std::string name(key);
    if (!object.contains(name))
      note(errors, "invalid_value", path_child(path, name));
  }
  return &object;
}

void check_string(const Object &object, std::string_view key,
                  const std::string &path, std::vector<RequestError> &errors,
                  std::string_view expected = {}) {
  const auto found = object.find(std::string(key));
  if (found == object.end())
    return;
  const auto field_path = path_child(path, key);
  if (!found->second.is_string())
    note(errors, "invalid_type", field_path);
  else if (!expected.empty() && found->second.string() != expected)
    note(errors, "invalid_value", field_path);
}

void check_uint(const Object &object, std::string_view key,
                const std::string &path, std::vector<RequestError> &errors,
                bool positive,
                std::optional<std::uint64_t> expected = std::nullopt) {
  const auto found = object.find(std::string(key));
  if (found == object.end())
    return;
  const auto field_path = path_child(path, key);
  if (!found->second.is_number() ||
      !unsigned_integer_lexeme(
          std::get<gemm_json::Number>(found->second.storage).lexeme)) {
    note(errors, "invalid_type", field_path);
    return;
  }
  const auto &lexeme =
      std::get<gemm_json::Number>(found->second.storage).lexeme;
  if (exceeds_safe_integer(lexeme)) {
    note(errors, "invalid_value", field_path);
    return;
  }
  const auto number = found->second.unsigned_integer();
  if ((positive && number == 0) || (expected && number != *expected))
    note(errors, "invalid_value", field_path);
}

void check_bool(const Object &object, std::string_view key,
                const std::string &path, std::vector<RequestError> &errors,
                bool expected) {
  const auto found = object.find(std::string(key));
  if (found == object.end())
    return;
  const auto field_path = path_child(path, key);
  if (!found->second.is_bool())
    note(errors, "invalid_type", field_path);
  else if (std::get<bool>(found->second.storage) != expected)
    note(errors, "invalid_value", field_path);
}

void validate_contract_fields(const Value &tree) {
  std::vector<RequestError> errors;
  const auto *root =
      collect_object(tree, "",
                     {"schema_id", "schema_version", "canonicalization_id",
                      "workload", "measurement", "mappings"},
                     {}, errors);
  if (root) {
    check_string(*root, "schema_id", "", errors, kSchema);
    check_uint(*root, "schema_version", "", errors, true, 1);
    check_string(*root, "canonicalization_id", "", errors, kCanonicalization);
    const auto wi = root->find("workload");
    if (wi != root->end()) {
      const std::string path = "/workload";
      const auto *workload = collect_object(
          wi->second, path,
          {"operation", "m", "n", "k", "batch_count", "input_format",
           "accumulator_output_format", "overflow", "rounding", "layout",
           "transpose_a", "transpose_b", "initial_c", "operand_data_identity"},
          {}, errors);
      if (workload) {
        check_string(*workload, "operation", path, errors, "gemm");
        check_uint(*workload, "m", path, errors, true);
        check_uint(*workload, "n", path, errors, true);
        check_uint(*workload, "k", path, errors, true);
        check_uint(*workload, "batch_count", path, errors, true, 1);
        check_string(*workload, "input_format", path, errors,
                     "signed_i16_twos_complement");
        check_string(*workload, "accumulator_output_format", path, errors,
                     "signed_i32_twos_complement");
        check_string(*workload, "overflow", path, errors, "wrap_mod_2pow32");
        check_string(*workload, "rounding", path, errors, "none");
        check_string(*workload, "layout", path, errors, "row_major_contiguous");
        check_bool(*workload, "transpose_a", path, errors, false);
        check_bool(*workload, "transpose_b", path, errors, false);
        check_string(*workload, "initial_c", path, errors, "zero");
        check_string(*workload, "operand_data_identity", path, errors,
                     "shape_only_no_values_v1");
      }
    }
    const auto mi = root->find("measurement");
    if (mi != root->end()) {
      const std::string path = "/measurement";
      const auto *measurement =
          collect_object(mi->second, path,
                         {"protocol", "repeat_count", "release_policy",
                          "unit_order", "clock_hz"},
                         {"backing_store_sensitivity"}, errors);
      if (measurement) {
        check_string(*measurement, "protocol", path, errors,
                     "finite_serial_batch_v1");
        check_uint(*measurement, "repeat_count", path, errors, true);
        check_string(*measurement, "release_policy", path, errors,
                     "all_at_zero");
        check_string(*measurement, "unit_order", path, errors,
                     "ascending_index_no_overlap");
        check_uint(*measurement, "clock_hz", path, errors, true,
                   kGemmV1ClockHz);
        const auto si = measurement->find("backing_store_sensitivity");
        if (si != measurement->end()) {
          const std::string sensitivity_path =
              "/measurement/backing_store_sensitivity";
          const auto *sensitivity = collect_object(
              si->second, sensitivity_path,
              {"bandwidth_bytes_per_second", "batch_setup_cycles"}, {}, errors);
          if (sensitivity) {
            check_uint(*sensitivity, "bandwidth_bytes_per_second",
                       sensitivity_path, errors, true);
            check_uint(*sensitivity, "batch_setup_cycles", sensitivity_path,
                       errors, false);
          }
        }
      }
    }
    const auto ai = root->find("mappings");
    if (ai != root->end()) {
      const std::string path = "/mappings";
      const auto *mappings =
          collect_object(ai->second, path, {"rvv", "matrix"}, {}, errors);
      if (mappings) {
        const auto ri = mappings->find("rvv");
        if (ri != mappings->end()) {
          const std::string rvv_path = "/mappings/rvv";
          const auto *rvv = collect_object(
              ri->second, rvv_path,
              {"kind", "vlen_bits", "sew_bits", "lmul", "lanes",
               "macs_per_lane_cycle", "accumulator_recurrence_cycles",
               "kernel_setup_cycles", "chunk_setup_cycles"},
              {}, errors);
          if (rvv) {
            check_string(*rvv, "kind", rvv_path, errors, "rvv_row_v1");
            check_uint(*rvv, "vlen_bits", rvv_path, errors, true);
            const auto vlen = rvv->find("vlen_bits");
            if (vlen != rvv->end() && vlen->second.is_unsigned_integer()) {
              const auto bits = vlen->second.unsigned_integer();
              if (bits < 128 || (bits & (bits - 1)) != 0)
                note(errors, "invalid_value",
                     path_child(rvv_path, "vlen_bits"));
            }
            check_uint(*rvv, "sew_bits", rvv_path, errors, true, 16);
            check_uint(*rvv, "lmul", rvv_path, errors, true, 1);
            check_uint(*rvv, "lanes", rvv_path, errors, true);
            check_uint(*rvv, "macs_per_lane_cycle", rvv_path, errors, true);
            check_uint(*rvv, "accumulator_recurrence_cycles", rvv_path, errors,
                       true);
            check_uint(*rvv, "kernel_setup_cycles", rvv_path, errors, false);
            check_uint(*rvv, "chunk_setup_cycles", rvv_path, errors, false);
          }
        }
        const auto xi = mappings->find("matrix");
        if (xi != mappings->end()) {
          const std::string matrix_path = "/mappings/matrix";
          const auto *matrix = collect_object(
              xi->second, matrix_path,
              {"kind", "tile_m", "tile_n", "tile_k", "local_capacity_bytes",
               "mac_slots_per_cycle", "accumulator_recurrence_cycles",
               "kernel_setup_cycles", "tile_setup_cycles"},
              {}, errors);
          if (matrix) {
            check_string(*matrix, "kind", matrix_path, errors,
                         "tiled_matrix_output_stationary_v1");
            check_uint(*matrix, "tile_m", matrix_path, errors, true);
            check_uint(*matrix, "tile_n", matrix_path, errors, true);
            check_uint(*matrix, "tile_k", matrix_path, errors, true);
            check_uint(*matrix, "local_capacity_bytes", matrix_path, errors,
                       true);
            check_uint(*matrix, "mac_slots_per_cycle", matrix_path, errors,
                       true);
            check_uint(*matrix, "accumulator_recurrence_cycles", matrix_path,
                       errors, true);
            check_uint(*matrix, "kernel_setup_cycles", matrix_path, errors,
                       false);
            check_uint(*matrix, "tile_setup_cycles", matrix_path, errors,
                       false);
          }
        }
      }
    }
  }
  if (!errors.empty()) {
    std::sort(errors.begin(), errors.end(),
              [](const RequestError &left, const RequestError &right) {
                return std::tie(left.path, left.code) <
                       std::tie(right.path, right.code);
              });
    throw errors.front();
  }
}

[[nodiscard]] Value parse_request_tree(std::string_view raw) {
  try {
    return gemm_json::parse(raw);
  } catch (const gemm_json::Error &error) {
    throw RequestError{error.code, error.path};
  }
}

[[nodiscard]] std::string error_envelope(std::string status, std::string scope,
                                         std::string code, std::string path) {
  Object item{{"code", string_value(std::move(code))},
              {"path", string_value(std::move(path))},
              {"scope", string_value(std::move(scope))}};
  Object envelope{{"errors", Value(Array{Value(std::move(item))})},
                  {"schema_id", string_value("axissim.gemm-pair.error")},
                  {"schema_version", uint_value(1)},
                  {"status", string_value(std::move(status))}};
  return gemm_json::canonical(Value(std::move(envelope)));
}

[[nodiscard]] std::string pre_envelope(std::string status, std::string code,
                                       std::string path) {
  return error_envelope(std::move(status), "request", std::move(code),
                        std::move(path));
}

[[nodiscard]] bool is_expected_schema(const Value &tree) {
  if (!tree.is_object())
    return false;
  const auto &root = tree.object();
  const auto id = root.find("schema_id");
  const auto version = root.find("schema_version");
  return id != root.end() && id->second.is_string() &&
         id->second.string() == kSchema && version != root.end() &&
         version->second.is_unsigned_integer() &&
         version->second.unsigned_integer() == 1;
}

[[nodiscard]] Request validate_request(Value tree) {
  validate_contract_fields(tree);
  if (!tree.is_object())
    bad("invalid_type", "");
  const auto &root = tree.object();
  exact_keys(root,
             {"schema_id", "schema_version", "canonicalization_id", "workload",
              "measurement", "mappings"},
             "");
  enum_field(root, "schema_id", "", kSchema);
  if (unsigned_field(root, "schema_version", "", true) != 1)
    bad("invalid_value", "/schema_version");
  enum_field(root, "canonicalization_id", "", kCanonicalization);

  Request request;
  request.tree = tree;
  request.workload_tree = field(root, "workload", "");
  request.measurement_tree = field(root, "measurement", "");
  request.workload = {};
  const auto &workload = as_object(request.workload_tree, "/workload");
  exact_keys(workload,
             {"operation", "m", "n", "k", "batch_count", "input_format",
              "accumulator_output_format", "overflow", "rounding", "layout",
              "transpose_a", "transpose_b", "initial_c",
              "operand_data_identity"},
             "/workload");
  enum_field(workload, "operation", "/workload", "gemm");
  request.workload.m = unsigned_field(workload, "m", "/workload", true);
  request.workload.n = unsigned_field(workload, "n", "/workload", true);
  request.workload.k = unsigned_field(workload, "k", "/workload", true);
  if (unsigned_field(workload, "batch_count", "/workload", true) != 1)
    bad("invalid_value", "/workload/batch_count");
  enum_field(workload, "input_format", "/workload",
             "signed_i16_twos_complement");
  enum_field(workload, "accumulator_output_format", "/workload",
             "signed_i32_twos_complement");
  enum_field(workload, "overflow", "/workload", "wrap_mod_2pow32");
  enum_field(workload, "rounding", "/workload", "none");
  enum_field(workload, "layout", "/workload", "row_major_contiguous");
  bool_field(workload, "transpose_a", "/workload", false);
  bool_field(workload, "transpose_b", "/workload", false);
  enum_field(workload, "initial_c", "/workload", "zero");
  enum_field(workload, "operand_data_identity", "/workload",
             "shape_only_no_values_v1");

  const auto &measurement = as_object(request.measurement_tree, "/measurement");
  exact_keys(measurement,
             {
                 "protocol",
                 "repeat_count",
                 "release_policy",
                 "unit_order",
                 "clock_hz",
             },
             "/measurement", {"backing_store_sensitivity"});
  enum_field(measurement, "protocol", "/measurement", "finite_serial_batch_v1");
  request.repeat_count =
      unsigned_field(measurement, "repeat_count", "/measurement", true);
  enum_field(measurement, "release_policy", "/measurement", "all_at_zero");
  enum_field(measurement, "unit_order", "/measurement",
             "ascending_index_no_overlap");
  request.clock_hz =
      unsigned_field(measurement, "clock_hz", "/measurement", true);
  if (request.clock_hz != kGemmV1ClockHz)
    bad("invalid_value", "/measurement/clock_hz");
  const auto sensitivity = measurement.find("backing_store_sensitivity");
  if (sensitivity != measurement.end()) {
    const auto &store = as_object(sensitivity->second,
                                  "/measurement/backing_store_sensitivity");
    exact_keys(store, {"bandwidth_bytes_per_second", "batch_setup_cycles"},
               "/measurement/backing_store_sensitivity");
    SerializedBackingStoreParameters params;
    params.bandwidth_bytes_per_second =
        unsigned_field(store, "bandwidth_bytes_per_second",
                       "/measurement/backing_store_sensitivity", true);
    params.batch_setup_cycles =
        unsigned_field(store, "batch_setup_cycles",
                       "/measurement/backing_store_sensitivity", false);
    request.backing_store = params;
  }

  const auto &mappings_root =
      as_object(field(root, "mappings", ""), "/mappings");
  exact_keys(mappings_root, {"rvv", "matrix"}, "/mappings");
  request.rvv_tree = field(mappings_root, "rvv", "/mappings");
  request.matrix_tree = field(mappings_root, "matrix", "/mappings");
  const auto &rvv = as_object(request.rvv_tree, "/mappings/rvv");
  exact_keys(rvv,
             {"kind", "vlen_bits", "sew_bits", "lmul", "lanes",
              "macs_per_lane_cycle", "accumulator_recurrence_cycles",
              "kernel_setup_cycles", "chunk_setup_cycles"},
             "/mappings/rvv");
  enum_field(rvv, "kind", "/mappings/rvv", "rvv_row_v1");
  request.rvv.vlen_bits =
      unsigned_field(rvv, "vlen_bits", "/mappings/rvv", true);
  if (request.rvv.vlen_bits < 128 ||
      (request.rvv.vlen_bits & (request.rvv.vlen_bits - 1)) != 0)
    bad("invalid_value", "/mappings/rvv/vlen_bits");
  if (unsigned_field(rvv, "sew_bits", "/mappings/rvv", true) != 16)
    bad("invalid_value", "/mappings/rvv/sew_bits");
  if (unsigned_field(rvv, "lmul", "/mappings/rvv", true) != 1)
    bad("invalid_value", "/mappings/rvv/lmul");
  request.rvv.lanes = unsigned_field(rvv, "lanes", "/mappings/rvv", true);
  request.rvv.macs_per_lane_cycle =
      unsigned_field(rvv, "macs_per_lane_cycle", "/mappings/rvv", true);
  request.rvv.accumulator_recurrence_cycles = unsigned_field(
      rvv, "accumulator_recurrence_cycles", "/mappings/rvv", true);
  request.rvv.kernel_setup_cycles =
      unsigned_field(rvv, "kernel_setup_cycles", "/mappings/rvv", false);
  request.rvv.chunk_setup_cycles =
      unsigned_field(rvv, "chunk_setup_cycles", "/mappings/rvv", false);

  const auto &matrix = as_object(request.matrix_tree, "/mappings/matrix");
  exact_keys(matrix,
             {"kind", "tile_m", "tile_n", "tile_k", "local_capacity_bytes",
              "mac_slots_per_cycle", "accumulator_recurrence_cycles",
              "kernel_setup_cycles", "tile_setup_cycles"},
             "/mappings/matrix");
  enum_field(matrix, "kind", "/mappings/matrix",
             "tiled_matrix_output_stationary_v1");
  request.matrix.tile_m =
      unsigned_field(matrix, "tile_m", "/mappings/matrix", true);
  request.matrix.tile_n =
      unsigned_field(matrix, "tile_n", "/mappings/matrix", true);
  request.matrix.tile_k =
      unsigned_field(matrix, "tile_k", "/mappings/matrix", true);
  request.matrix.local_capacity_bytes =
      unsigned_field(matrix, "local_capacity_bytes", "/mappings/matrix", true);
  request.matrix.mac_slots_per_cycle =
      unsigned_field(matrix, "mac_slots_per_cycle", "/mappings/matrix", true);
  request.matrix.accumulator_recurrence_cycles = unsigned_field(
      matrix, "accumulator_recurrence_cycles", "/mappings/matrix", true);
  request.matrix.kernel_setup_cycles =
      unsigned_field(matrix, "kernel_setup_cycles", "/mappings/matrix", false);
  request.matrix.tile_setup_cycles =
      unsigned_field(matrix, "tile_setup_cycles", "/mappings/matrix", false);

  try {
    validate_gemm_workload(request.workload);
  } catch (const std::overflow_error &) {
    bad("checked_overflow", "/workload");
  } catch (const std::exception &) {
    bad("invalid_value", "/workload");
  }
  try {
    preflight_limits(request);
  } catch (const RequestError &) {
    throw;
  } catch (const std::overflow_error &) {
    bad("checked_overflow", "/workload");
  } catch (const std::exception &) {
    bad("invalid_value", "/mappings");
  }

  request.request_sha = hex_digest(gemm_json::canonical(request.tree));
  request.workload_id = hex_digest(gemm_json::canonical(request.workload_tree));
  request.rvv_id = hex_digest(gemm_json::canonical(request.rvv_tree));
  request.matrix_id = hex_digest(gemm_json::canonical(request.matrix_tree));
  const Value pair_identity = object_value(
      {{"schema_id", string_value(std::string(kSchema))},
       {"schema_version", uint_value(1)},
       {"workload_id", string_value(request.workload_id)},
       {"measurement", request.measurement_tree},
       {"mappings", object_value({{"matrix", string_value(request.matrix_id)},
                                  {"rvv", string_value(request.rvv_id)}})}});
  request.pair_id = hex_digest(gemm_json::canonical(pair_identity));
  const Value assumptions = object_value(
      {{"schema_id", string_value("axissim.gemm-pair.model-assumptions")},
       {"schema_version", uint_value(1)},
       {"workload_id", string_value(request.workload_id)},
       {"measurement", request.measurement_tree},
       {"model_semantics",
        object_value(
            {{"matrix",
              string_value("axissim.tiled_matrix_output_stationary_v1")},
             {"rvv", string_value("axissim.rvv_row_v1")}})},
       {"mappings", object_value({{"matrix", request.matrix_tree},
                                  {"rvv", request.rvv_tree}})}});
  request.assumptions_sha = hex_digest(gemm_json::canonical(assumptions));
  return request;
}

[[nodiscard]] Value rational_cycles(std::uint64_t cycles, std::uint64_t hz) {
  return object_value({{"denominator_hz", uint_value(hz)},
                       {"numerator_cycles", uint_value(cycles)}});
}

[[nodiscard]] Value rate(std::uint64_t hz, std::uint64_t cycles_per_unit) {
  return object_value(
      {{"denominator_cycles_per_unit", uint_value(cycles_per_unit)},
       {"numerator_hz", uint_value(hz)}});
}

[[nodiscard]] Value p99(std::uint64_t cycles, std::uint64_t hz,
                        std::uint64_t rank) {
  return object_value({{"denominator_hz", uint_value(hz)},
                       {"numerator_cycles", uint_value(cycles)},
                       {"p99_rank", uint_value(rank)}});
}

[[nodiscard]] std::uint64_t nearest_rank_99(std::uint64_t count) {
  // ceil(0.99 * count), written without floating-point arithmetic.
  return (count / 100) * 99 + ((count % 100) * 99 + 99) / 100;
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b,
                                             std::string_view path) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
    bad("checked_overflow", std::string(path));
  return a * b;
}

[[nodiscard]] std::uint64_t checked_add(std::uint64_t a, std::uint64_t b,
                                        std::string_view path) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    bad("checked_overflow", std::string(path));
  return a + b;
}

void preflight_limits(const Request &request) {
  const auto counts = count_gemm_work(request.workload);
  if (checked_multiply(counts.useful_macs, request.repeat_count,
                       "/measurement/repeat_count") > kMaxUsefulMacs)
    bad("checked_overflow", "/measurement/repeat_count");
  const auto rvv = estimate_rvv_row_gemm(request.workload, request.rvv);
  auto unbounded_matrix_mapping = request.matrix;
  unbounded_matrix_mapping.local_capacity_bytes =
      std::numeric_limits<std::uint64_t>::max();
  const auto matrix =
      estimate_tiled_matrix_gemm(request.workload, unbounded_matrix_mapping);
  if (checked_multiply(rvv.vector_mac_instructions, request.repeat_count,
                       "/mappings/rvv") > kMaxMappingOperations)
    bad("checked_overflow", "/mappings/rvv");
  if (checked_multiply(matrix.matrix_tile_operations, request.repeat_count,
                       "/mappings/matrix") > kMaxMappingOperations)
    bad("checked_overflow", "/mappings/matrix");
  const auto rvv_bytes =
      checked_multiply(rvv.logical_traffic.total_bytes, request.repeat_count,
                       "/measurement/repeat_count");
  const auto matrix_bytes =
      checked_multiply(matrix.logical_traffic.total_bytes, request.repeat_count,
                       "/measurement/repeat_count");
  if (checked_add(rvv_bytes, matrix_bytes, "/measurement/repeat_count") >
      kMaxLogicalBytes)
    bad("checked_overflow", "/measurement/repeat_count");
  const auto check_timing = [&](std::uint64_t cycles_per_unit,
                                const GemmLogicalTraffic &traffic,
                                std::uint64_t slots_per_cycle,
                                std::string_view path) {
    const auto total_cycles =
        checked_multiply(cycles_per_unit, request.repeat_count, path);
    const auto p99_cycles = checked_multiply(
        cycles_per_unit, nearest_rank_99(request.repeat_count), path);
    (void)checked_multiply(slots_per_cycle, total_cycles, path);
    if (cycles_per_unit > kMaxCycles || total_cycles > kMaxCycles ||
        p99_cycles > kMaxCycles)
      bad("checked_overflow", std::string(path));
    if (request.backing_store) {
      const auto serialized = estimate_serialized_backing_store(
          traffic, cycles_per_unit, *request.backing_store);
      const auto serial_total =
          checked_multiply(serialized.total_cycles, request.repeat_count, path);
      const auto serial_p99 = checked_multiply(
          serialized.total_cycles, nearest_rank_99(request.repeat_count), path);
      if (serialized.total_cycles > kMaxCycles || serial_total > kMaxCycles ||
          serial_p99 > kMaxCycles)
        bad("checked_overflow", std::string(path));
    }
  };
  check_timing(rvv.compute_cycles, rvv.logical_traffic, rvv.mac_slots_per_cycle,
               "/mappings/rvv");
  check_timing(matrix.compute_cycles, matrix.logical_traffic,
               matrix.mac_slots_per_cycle, "/mappings/matrix");
}

[[nodiscard]] Value traffic_value(const GemmLogicalTraffic &per,
                                  std::uint64_t repeats) {
  const auto mul = [&](std::uint64_t value) {
    if (value != 0 &&
        repeats > std::numeric_limits<std::uint64_t>::max() / value)
      throw std::overflow_error("traffic repeat overflow");
    return value * repeats;
  };
  return object_value(
      {{"read_a_bytes_per_unit", uint_value(per.read_a_bytes)},
       {"read_a_bytes_total", uint_value(mul(per.read_a_bytes))},
       {"read_b_bytes_per_unit", uint_value(per.read_b_bytes)},
       {"read_b_bytes_total", uint_value(mul(per.read_b_bytes))},
       {"write_c_bytes_per_unit", uint_value(per.write_c_bytes)},
       {"write_c_bytes_total", uint_value(mul(per.write_c_bytes))},
       {"total_bytes_per_unit", uint_value(per.total_bytes)},
       {"total_bytes_total", uint_value(mul(per.total_bytes))},
       {"read_a_batches_per_unit", uint_value(per.read_a_batches)},
       {"read_a_batches_total", uint_value(mul(per.read_a_batches))},
       {"read_b_batches_per_unit", uint_value(per.read_b_batches)},
       {"read_b_batches_total", uint_value(mul(per.read_b_batches))},
       {"write_c_batches_per_unit", uint_value(per.write_c_batches)},
       {"write_c_batches_total", uint_value(mul(per.write_c_batches))}});
}

[[nodiscard]] Value
optional_serialized(const GemmLogicalTraffic &traffic,
                    std::uint64_t compute_cycles, std::uint64_t repeat_count,
                    const SerializedBackingStoreParameters &params) {
  const auto estimate =
      estimate_serialized_backing_store(traffic, compute_cycles, params);
  const auto total = estimate.total_cycles;
  const auto makespan = total * repeat_count;
  const auto rank = nearest_rank_99(repeat_count);
  const auto p99_cycles = total * rank;
  if (total > kMaxCycles || makespan > kMaxCycles || p99_cycles > kMaxCycles)
    throw std::overflow_error(
        "serialized timing result exceeds signed 64-bit limit");
  return object_value(
      {{"read_cycles_per_unit", uint_value(estimate.read_cycles)},
       {"compute_cycles_per_unit", uint_value(estimate.compute_cycles)},
       {"write_cycles_per_unit", uint_value(estimate.write_cycles)},
       {"total_cycles_per_unit", uint_value(total)},
       {"finite_makespan_cycles", uint_value(makespan)},
       {"finite_batch_rate", rate(kGemmV1ClockHz, total)},
       {"p99_completion", p99(p99_cycles, kGemmV1ClockHz, rank)}});
}

[[nodiscard]] Value complete_arm(const Request &request, bool rvv_arm) {
  const auto counts = count_gemm_work(request.workload);
  const auto total_macs = counts.useful_macs * request.repeat_count;
  const auto total_ops =
      counts.conventional_operation_equivalents * request.repeat_count;
  if (total_macs > kMaxUsefulMacs)
    throw RequestError{"checked_overflow", "/measurement/repeat_count"};
  std::uint64_t per_cycles = 0;
  std::uint64_t slots_per_cycle = 0;
  std::uint64_t mapping_operations = 0;
  std::uint64_t working_set = 0;
  GemmLogicalTraffic traffic;
  std::string mapping_id;
  std::string mapping_kind;
  Object operation_count;
  if (rvv_arm) {
    const auto estimate = estimate_rvv_row_gemm(request.workload, request.rvv);
    per_cycles = estimate.compute_cycles;
    slots_per_cycle = estimate.mac_slots_per_cycle;
    mapping_operations = estimate.vector_mac_instructions;
    traffic = estimate.logical_traffic;
    mapping_id = request.rvv_id;
    mapping_kind = "rvv_row_v1";
    operation_count = {
        {"vector_mac_instructions_per_unit", uint_value(mapping_operations)},
        {"vector_mac_instructions_total",
         uint_value(mapping_operations * request.repeat_count)}};
    if (mapping_operations * request.repeat_count > kMaxMappingOperations)
      throw RequestError{"checked_overflow", "/mappings/rvv"};
  } else {
    const auto estimate =
        estimate_tiled_matrix_gemm(request.workload, request.matrix);
    per_cycles = estimate.compute_cycles;
    slots_per_cycle = estimate.mac_slots_per_cycle;
    mapping_operations = estimate.matrix_tile_operations;
    traffic = estimate.logical_traffic;
    working_set = estimate.working_set_bytes;
    mapping_id = request.matrix_id;
    mapping_kind = "tiled_matrix_output_stationary_v1";
    operation_count = {
        {"matrix_tile_operations_per_unit", uint_value(mapping_operations)},
        {"matrix_tile_operations_total",
         uint_value(mapping_operations * request.repeat_count)}};
    if (mapping_operations * request.repeat_count > kMaxMappingOperations)
      throw RequestError{"checked_overflow", "/mappings/matrix"};
  }
  const auto total_cycles = per_cycles * request.repeat_count;
  const auto rank = nearest_rank_99(request.repeat_count);
  const auto p99_cycles = per_cycles * rank;
  const auto available_slots = slots_per_cycle * total_cycles;
  if (per_cycles > kMaxCycles || total_cycles > kMaxCycles ||
      p99_cycles > kMaxCycles || available_slots > kMaxCycles)
    throw RequestError{"checked_overflow", "/measurement/repeat_count"};
  if (traffic.total_bytes > kMaxLogicalBytes ||
      traffic.total_bytes * request.repeat_count > kMaxLogicalBytes)
    throw RequestError{"checked_overflow", "/measurement/repeat_count"};

  Object workload_counts{
      {"useful_macs_per_unit", uint_value(counts.useful_macs)},
      {"useful_macs_total", uint_value(total_macs)},
      {"conventional_ops_per_unit",
       uint_value(counts.conventional_operation_equivalents)},
      {"conventional_ops_total", uint_value(total_ops)}};
  Object arm{
      {"arm_status", string_value("complete")},
      {"mapping_id", string_value(mapping_id)},
      {"mapping_kind", string_value(mapping_kind)},
      {"workload_counts", Value(std::move(workload_counts))},
      {"logical_traffic", traffic_value(traffic, request.repeat_count)},
      {"compute_cycles_per_unit", uint_value(per_cycles)},
      {"compute_cycles_total", uint_value(total_cycles)},
      {"compute_duration_seconds",
       rational_cycles(per_cycles, request.clock_hz)},
      {"finite_compute_makespan_seconds",
       rational_cycles(total_cycles, request.clock_hz)},
      {"finite_batch_rate", rate(request.clock_hz, per_cycles)},
      {"p99_compute_completion", p99(p99_cycles, request.clock_hz, rank)},
      {"modeled_mac_slot_utilization",
       object_value({{"available_mac_slots", uint_value(available_slots)},
                     {"useful_macs", uint_value(total_macs)}})}};
  for (auto &[key, value] : operation_count)
    arm.emplace(key, std::move(value));
  if (!rvv_arm) {
    arm.emplace("working_set_bytes", uint_value(working_set));
    arm.emplace("capacity_fit", Value(true));
  }
  if (request.backing_store) {
    arm.emplace("serialized_backing_store",
                optional_serialized(traffic, per_cycles, request.repeat_count,
                                    *request.backing_store));
  }
  return Value(std::move(arm));
}

[[nodiscard]] Value arm_failure(std::string status) {
  return object_value({{"arm_status", string_value(std::move(status))}});
}

[[nodiscard]] Value error_value(std::string scope, std::string code,
                                std::string path) {
  return object_value({{"scope", string_value(std::move(scope))},
                       {"code", string_value(std::move(code))},
                       {"path", string_value(std::move(path))}});
}

[[nodiscard]] Value model_coverage(bool has_sensitivity) {
  return object_value(
      {{"compute_timing", string_value("analytical_assumption")},
       {"logical_traffic", string_value("mapping_derived_payload_only")},
       {"physical_memory_actions", string_value("not_modeled")},
       {"memory_service",
        string_value(has_sensitivity ? "serialized_sensitivity_only"
                                     : "not_requested")},
       {"scheduling", string_value("serial_batch_no_arbitration")},
       {"host_dispatch", string_value("excluded")},
       {"overlap", string_value("excluded")},
       {"energy", string_value("not_modeled")},
       {"power", string_value("not_modeled")},
       {"area", string_value("not_modeled")},
       {"AME_semantics", string_value("not_implemented")}});
}

[[nodiscard]] std::string file_sha256(const std::string &path) {
  if (path.empty())
    return "unknown";
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return "unknown";
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof())
    return "unknown";
  return sha256_hex(bytes);
}

[[nodiscard]] Value simulator_identity(const std::string &executable_path) {
  return object_value(
      {{"source_revision", string_value("unknown")},
       {"source_tree_state", string_value("unknown")},
       {"executable_sha256", string_value(file_sha256(executable_path))},
       {"model_semantics_id",
        string_value(
            "axissim.rvv_row_v1+axissim.tiled_matrix_output_stationary_v1")},
       {"compiler_id", string_value("unknown")},
       {"compiler_version", string_value("unknown")},
       {"build_configuration_id", string_value("unknown")},
       {"consumed_inputs", Value(Array{})}});
}

[[nodiscard]] Value measurement_result(const Request &request) {
  Object result{
      {"protocol", string_value("finite_serial_batch_v1")},
      {"release_policy", string_value("all_at_zero")},
      {"unit_order", string_value("ascending_index_no_overlap")},
      {"clock_hz", uint_value(request.clock_hz)},
      {"primary_timing_scope", string_value("compute_only")},
      {"primary_completion_boundary", string_value("last_unit_compute")},
      {"overlap_policy", string_value("none")},
      {"p99_method", string_value("nearest_rank_one_based")}};
  if (request.backing_store) {
    result.emplace(
        "backing_store_sensitivity",
        object_value(
            {{"bandwidth_bytes_per_second",
              uint_value(request.backing_store->bandwidth_bytes_per_second)},
             {"batch_setup_cycles",
              uint_value(request.backing_store->batch_setup_cycles)}}));
  }
  return Value(std::move(result));
}

[[nodiscard]] Value result_record(const Request &request,
                                  const std::string &executable_path,
                                  Value rvv_arm, Value matrix_arm,
                                  std::vector<Value> errors,
                                  std::string status) {
  std::sort(errors.begin(), errors.end(),
            [](const Value &left, const Value &right) {
              const auto &l = left.object();
              const auto &r = right.object();
              return std::tuple{l.at("scope").string(), l.at("code").string(),
                                l.at("path").string()} <
                     std::tuple{r.at("scope").string(), r.at("code").string(),
                                r.at("path").string()};
            });
  return object_value(
      {{"schema_id", string_value("axissim.gemm-pair.result")},
       {"schema_version", uint_value(1)},
       {"canonicalization_id", string_value(std::string(kCanonicalization))},
       {"candidate_pair_id", string_value(request.pair_id)},
       {"request_sha256", string_value(request.request_sha)},
       {"model_assumptions_sha256", string_value(request.assumptions_sha)},
       {"simulator", simulator_identity(executable_path)},
       {"workload_id", string_value(request.workload_id)},
       {"measurement_protocol", measurement_result(request)},
       {"repeat_count", uint_value(request.repeat_count)},
       {"status", string_value(std::move(status))},
       {"arms", object_value({{"matrix", std::move(matrix_arm)},
                              {"rvv", std::move(rvv_arm)}})},
       {"model_coverage", model_coverage(request.backing_store.has_value())},
       {"errors", Value(std::move(errors))},
       {"energy_status", string_value("not_modeled")},
       {"power_status", string_value("not_modeled")},
       {"budget_status", string_value("not_evaluated")},
       {"physical_feasibility", string_value("not_evaluated")},
       {"uncertainty_class",
        string_value("assumption_limited_deterministic_model")}});
}

} // namespace

GemmPairResponse evaluate_study_gemm_pair(std::string_view raw,
                                          const std::string &executable_path) {
  if (raw.size() > kMaxInputBytes)
    return {pre_envelope("input_limit", "request_too_large", ""), 2};
  Value tree;
  try {
    tree = parse_request_tree(raw);
  } catch (const RequestError &error) {
    return {pre_envelope("invalid_json", error.code, error.path), 2};
  }
  if (!is_expected_schema(tree))
    return {pre_envelope("unsupported_schema", "unsupported_schema", ""), 2};
  try {
    validate_ascii_and_canonical_types(tree, "");
  } catch (const RequestError &error) {
    return {pre_envelope("invalid_request", error.code, error.path), 2};
  }

  Request request;
  try {
    request = validate_request(std::move(tree));
  } catch (const RequestError &error) {
    return {pre_envelope("invalid_request", error.code, error.path), 2};
  } catch (const std::overflow_error &) {
    return {pre_envelope("invalid_request", "checked_overflow", ""), 2};
  } catch (const std::exception &) {
    return {error_envelope("internal_error", "global", "internal_error", ""),
            4};
  }

  try {
    Value rvv_arm = arm_failure("not_run");
    Value matrix_arm = arm_failure("not_run");
    std::vector<Value> errors;
    bool rvv_complete = false;
    bool matrix_complete = false;
    try {
      rvv_arm = complete_arm(request, true);
      rvv_complete = true;
    } catch (const RequestError &error) {
      errors.push_back(error_value("rvv", "arm_execution_error", error.path));
      rvv_arm = arm_failure("failed");
    } catch (const std::exception &) {
      errors.push_back(
          error_value("rvv", "arm_execution_error", "/mappings/rvv"));
      rvv_arm = arm_failure("failed");
    }
    try {
      matrix_arm = complete_arm(request, false);
      matrix_complete = true;
    } catch (const std::length_error &) {
      errors.push_back(error_value("matrix", "capacity_exceeded",
                                   "/mappings/matrix/local_capacity_bytes"));
      matrix_arm = arm_failure("invalid");
    } catch (const RequestError &error) {
      errors.push_back(
          error_value("matrix", "arm_execution_error", error.path));
      matrix_arm = arm_failure("failed");
    } catch (const std::exception &) {
      errors.push_back(
          error_value("matrix", "arm_execution_error", "/mappings/matrix"));
      matrix_arm = arm_failure("failed");
    }
    const std::string status =
        rvv_complete && matrix_complete
            ? "complete"
            : (rvv_complete || matrix_complete ? "partial" : "failed");
    auto record =
        result_record(request, executable_path, std::move(rvv_arm),
                      std::move(matrix_arm), std::move(errors), status);
    std::string serialized = gemm_json::canonical(record);
    if (serialized.size() > kMaxOutputBytes) {
      Object envelope{
          {"candidate_pair_id", string_value(request.pair_id)},
          {"errors", Value(Array{error_value("global", "output_limit", "")})},
          {"model_assumptions_sha256", string_value(request.assumptions_sha)},
          {"request_sha256", string_value(request.request_sha)},
          {"schema_id", string_value("axissim.gemm-pair.error")},
          {"schema_version", uint_value(1)},
          {"status", string_value("output_limit")}};
      return {gemm_json::canonical(Value(std::move(envelope))), 5};
    }
    const int exit = status == "complete" ? 0 : (status == "partial" ? 3 : 4);
    return {std::move(serialized), exit};
  } catch (const std::exception &) {
    try {
      auto record = result_record(
          request, executable_path, arm_failure("not_run"),
          arm_failure("not_run"), {error_value("global", "internal_error", "")},
          "failed");
      auto serialized = gemm_json::canonical(record);
      if (serialized.size() <= kMaxOutputBytes)
        return {std::move(serialized), 4};
    } catch (const std::exception &) {
      // Fall through to the minimal error record if the candidate result
      // cannot be constructed or serialized.
    }
    return {error_envelope("internal_error", "global", "internal_error", ""),
            4};
  } catch (...) {
    return {error_envelope("internal_error", "global", "internal_error", ""),
            4};
  }
}

int run_study_gemm_pair(const std::string &request_path,
                        const std::string &result_path,
                        const std::string &executable_path) {
  std::string request;
  try {
    std::ifstream input(request_path, std::ios::binary);
    if (!input)
      return 4;
    request.resize(kMaxInputBytes + 1);
    input.read(request.data(), static_cast<std::streamsize>(request.size()));
    if (input.bad())
      return 4;
    request.resize(static_cast<std::size_t>(input.gcount()));
  } catch (const std::exception &) {
    return 4;
  }
  const auto response = evaluate_study_gemm_pair(request, executable_path);
  try {
    std::ofstream output(result_path, std::ios::binary | std::ios::trunc);
    if (!output)
      return 4;
    output.write(response.json.data(),
                 static_cast<std::streamsize>(response.json.size()));
    output.put('\n');
    output.flush();
    if (!output)
      return 4;
  } catch (const std::exception &) {
    return 4;
  }
  return response.exit_code;
}

} // namespace axissim
