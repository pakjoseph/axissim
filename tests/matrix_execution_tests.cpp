#include "matrix_execution.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using axissim::GemmAccumulatorOutputFormat;
using axissim::GemmInputFormat;
using axissim::GemmInitialC;
using axissim::GemmLayout;
using axissim::GemmOverflow;
using axissim::GemmRounding;
using axissim::GemmWorkload;
using axissim::MatrixBankCapacityInputs;
using axissim::MatrixCommand;
using axissim::MatrixCommandCounts;
using axissim::MatrixCommandIssueCosts;
using axissim::MatrixExecutionRequest;
using axissim::MatrixExecutionResult;
using axissim::MatrixExecutionStatus;
using axissim::MatrixMappingInputs;
using axissim::MatrixOpcode;
using axissim::MatrixSerializedInputs;
using axissim::RvvMappingInputs;

static_assert(!std::is_default_constructible_v<RvvMappingInputs>);
static_assert(!std::is_default_constructible_v<MatrixMappingInputs>);
static_assert(!std::is_default_constructible_v<MatrixBankCapacityInputs>);
static_assert(!std::is_default_constructible_v<MatrixCommandIssueCosts>);
static_assert(!std::is_default_constructible_v<MatrixSerializedInputs>);
static_assert(!std::is_default_constructible_v<MatrixExecutionRequest>);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_eq(std::uint64_t actual, std::uint64_t expected,
                const std::string& message) {
  require(actual == expected,
          message + " (expected " + std::to_string(expected) + ", got " +
              std::to_string(actual) + ")");
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

GemmWorkload workload(std::uint64_t m, std::uint64_t n, std::uint64_t k) {
  return {m, n, k,
          GemmInputFormat::signed_i16_twos_complement,
          GemmAccumulatorOutputFormat::signed_i32_twos_complement,
          GemmLayout::row_major_contiguous,
          GemmOverflow::wrap_modulo_2pow32,
          GemmRounding::none,
          GemmInitialC::zero};
}

MatrixExecutionRequest request(
    GemmWorkload work, std::uint64_t slots, std::uint64_t tile_m,
    std::uint64_t tile_n, std::uint64_t tile_k, std::uint64_t aggregate_capacity,
    std::uint64_t bank_a, std::uint64_t bank_b, std::uint64_t bank_acc,
    MatrixCommandIssueCosts costs,
    std::optional<MatrixSerializedInputs> serialized) {
  return MatrixExecutionRequest(
      work, axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, slots, 1, 1, 0, 0),
      MatrixMappingInputs(tile_m, tile_n, tile_k, aggregate_capacity, slots,
                          1, 0, 0),
      MatrixBankCapacityInputs(bank_a, bank_b, bank_acc), costs,
      serialized);
}

void require_no_matrix_cycles(const MatrixExecutionResult& result,
                              const std::string& context) {
  require(!result.command_issue_cycles.has_value(),
          context + " must not expose issue cycles");
  require(!result.combined_matrix_command_compute_cycles.has_value(),
          context + " must not expose a valid M7 interval");
  require(!result.rvv_serialized.has_value() &&
              !result.matrix_serialized.has_value(),
          context + " must not expose serialized comparison fields");
}

std::vector<MatrixCommand> canonical_test_schedule(
    std::uint64_t m, std::uint64_t n, std::uint64_t k,
    std::uint64_t tile_m, std::uint64_t tile_n, std::uint64_t tile_k) {
  const auto ceil_div = [](std::uint64_t value, std::uint64_t divisor) {
    return value / divisor + static_cast<std::uint64_t>(value % divisor != 0);
  };
  std::vector<MatrixCommand> commands;
  std::uint64_t sequence = 0;
  for (std::uint64_t im = 0; im < ceil_div(m, tile_m); ++im) {
    const auto valid_m = std::min(tile_m, m - im * tile_m);
    for (std::uint64_t in = 0; in < ceil_div(n, tile_n); ++in) {
      const auto valid_n = std::min(tile_n, n - in * tile_n);
      commands.push_back({sequence++, MatrixOpcode::clear_accumulator,
                          im, in, 0, valid_m, valid_n, 0});
      for (std::uint64_t ik = 0; ik < ceil_div(k, tile_k); ++ik) {
        const auto valid_k = std::min(tile_k, k - ik * tile_k);
        commands.push_back({sequence++, MatrixOpcode::load_a,
                            im, in, ik, valid_m, 0, valid_k});
        commands.push_back({sequence++, MatrixOpcode::load_b,
                            im, in, ik, 0, valid_n, valid_k});
        commands.push_back({sequence++, MatrixOpcode::matrix_mac_accumulate,
                            im, in, ik, valid_m, valid_n, valid_k});
      }
      commands.push_back({sequence++, MatrixOpcode::store_accumulator_tile,
                          im, in, 0, valid_m, valid_n, 0});
    }
  }
  return commands;
}

MatrixCommandCounts count_test_schedule(const std::vector<MatrixCommand>& commands) {
  const auto count = [&commands](MatrixOpcode opcode) {
    return static_cast<std::uint64_t>(std::count_if(
        commands.begin(), commands.end(),
        [opcode](const MatrixCommand& command) { return command.opcode == opcode; }));
  };
  return {count(MatrixOpcode::clear_accumulator),
          count(MatrixOpcode::load_a), count(MatrixOpcode::load_b),
          count(MatrixOpcode::matrix_mac_accumulate),
          count(MatrixOpcode::store_accumulator_tile)};
}

void require_counts(const MatrixCommandCounts& actual,
                    const MatrixCommandCounts& expected,
                    const std::string& context) {
  require_eq(actual.clear_accumulator, expected.clear_accumulator,
             context + " clear count");
  require_eq(actual.load_a, expected.load_a, context + " load A count");
  require_eq(actual.load_b, expected.load_b, context + " load B count");
  require_eq(actual.mac_accumulate, expected.mac_accumulate,
             context + " MAC count");
  require_eq(actual.store_accumulator_tile,
             expected.store_accumulator_tile, context + " store count");
}

bool same_work(const axissim::GemmWorkCounts& a,
               const axissim::GemmWorkCounts& b) {
  return a.useful_macs == b.useful_macs &&
         a.conventional_operation_equivalents ==
             b.conventional_operation_equivalents &&
         a.tensor_a_bytes == b.tensor_a_bytes &&
         a.tensor_b_bytes == b.tensor_b_bytes &&
         a.tensor_c_bytes == b.tensor_c_bytes &&
         a.unique_tensor_payload_bytes == b.unique_tensor_payload_bytes;
}

bool same_traffic(const axissim::GemmLogicalTraffic& a,
                  const axissim::GemmLogicalTraffic& b) {
  return a.read_a_bytes == b.read_a_bytes &&
         a.read_b_bytes == b.read_b_bytes &&
         a.write_c_bytes == b.write_c_bytes && a.total_bytes == b.total_bytes &&
         a.read_a_batches == b.read_a_batches &&
         a.read_b_batches == b.read_b_batches &&
         a.write_c_batches == b.write_c_batches;
}

bool same_rvv(const axissim::RvvGemmEstimate& a,
              const axissim::RvvGemmEstimate& b) {
  return same_work(a.work, b.work) && same_traffic(a.logical_traffic, b.logical_traffic) &&
         a.vlmax == b.vlmax && a.mac_slots_per_cycle == b.mac_slots_per_cycle &&
         a.vector_mac_instructions == b.vector_mac_instructions &&
         a.compute_cycles == b.compute_cycles;
}

bool same_matrix(const axissim::TiledMatrixGemmEstimate& a,
                 const axissim::TiledMatrixGemmEstimate& b) {
  return same_work(a.work, b.work) && same_traffic(a.logical_traffic, b.logical_traffic) &&
         a.tiles_m == b.tiles_m && a.tiles_n == b.tiles_n &&
         a.tiles_k == b.tiles_k &&
         a.matrix_tile_operations == b.matrix_tile_operations &&
         a.working_set_bytes == b.working_set_bytes &&
         a.local_capacity_bytes == b.local_capacity_bytes &&
         a.mac_slots_per_cycle == b.mac_slots_per_cycle &&
         a.compute_cycles == b.compute_cycles;
}

bool same_issue(const axissim::MatrixCommandIssueCycles& a,
                const axissim::MatrixCommandIssueCycles& b) {
  return a.clear_accumulator == b.clear_accumulator && a.load_a == b.load_a &&
         a.load_b == b.load_b && a.mac_accumulate == b.mac_accumulate &&
         a.store_accumulator_tile == b.store_accumulator_tile &&
         a.total == b.total;
}

bool same_serialized(const axissim::MatrixSerializedCycles& a,
                     const axissim::MatrixSerializedCycles& b) {
  return a.read_cycles == b.read_cycles &&
         a.serialized_engine_service_cycles ==
             b.serialized_engine_service_cycles &&
         a.write_cycles == b.write_cycles && a.total_cycles == b.total_cycles;
}

bool same_request(const MatrixExecutionRequest& a,
                  const MatrixExecutionRequest& b) {
  const auto& aw = a.workload;
  const auto& bw = b.workload;
  const auto& ar = a.rvv_mapping;
  const auto& br = b.rvv_mapping;
  const auto& am = a.matrix_mapping;
  const auto& bm = b.matrix_mapping;
  const auto& ac = a.matrix_bank_capacities;
  const auto& bc = b.matrix_bank_capacities;
  const auto& ai = a.command_issue_costs;
  const auto& bi = b.command_issue_costs;
  if (aw.m != bw.m || aw.n != bw.n || aw.k != bw.k ||
      aw.input_format != bw.input_format ||
      aw.accumulator_output_format != bw.accumulator_output_format ||
      aw.layout != bw.layout || aw.overflow != bw.overflow ||
      aw.rounding != bw.rounding || aw.initial_c != bw.initial_c ||
      a.reference_clock_hz != b.reference_clock_hz ||
      ar.vlen_bits != br.vlen_bits || ar.lanes != br.lanes ||
      ar.macs_per_lane_cycle != br.macs_per_lane_cycle ||
      ar.accumulator_recurrence_cycles != br.accumulator_recurrence_cycles ||
      ar.kernel_setup_cycles != br.kernel_setup_cycles ||
      ar.chunk_setup_cycles != br.chunk_setup_cycles ||
      am.tile_m != bm.tile_m || am.tile_n != bm.tile_n || am.tile_k != bm.tile_k ||
      am.local_capacity_bytes != bm.local_capacity_bytes ||
      am.mac_slots_per_cycle != bm.mac_slots_per_cycle ||
      am.accumulator_recurrence_cycles != bm.accumulator_recurrence_cycles ||
      am.kernel_setup_cycles != bm.kernel_setup_cycles ||
      am.tile_setup_cycles != bm.tile_setup_cycles ||
      ac.a_bytes != bc.a_bytes || ac.b_bytes != bc.b_bytes ||
      ac.accumulator_bytes != bc.accumulator_bytes ||
      ai.clear_accumulator_cycles_per_command !=
          bi.clear_accumulator_cycles_per_command ||
      ai.load_a_cycles_per_command != bi.load_a_cycles_per_command ||
      ai.load_b_cycles_per_command != bi.load_b_cycles_per_command ||
      ai.mac_accumulate_cycles_per_command !=
          bi.mac_accumulate_cycles_per_command ||
      ai.store_accumulator_tile_cycles_per_command !=
          bi.store_accumulator_tile_cycles_per_command ||
      a.serialized_parameters.has_value() != b.serialized_parameters.has_value()) {
    return false;
  }
  return !a.serialized_parameters.has_value() ||
         (a.serialized_parameters->bandwidth_bytes_per_second ==
              b.serialized_parameters->bandwidth_bytes_per_second &&
          a.serialized_parameters->batch_setup_cycles ==
              b.serialized_parameters->batch_setup_cycles);
}

bool same_result(const MatrixExecutionResult& a,
                 const MatrixExecutionResult& b) {
  if (a.status != b.status || !same_request(a.evaluated_request, b.evaluated_request) ||
      !same_rvv(a.rvv_estimate, b.rvv_estimate) ||
      a.matrix_bank_fit.required_bytes.a_bytes !=
          b.matrix_bank_fit.required_bytes.a_bytes ||
      a.matrix_bank_fit.required_bytes.b_bytes !=
          b.matrix_bank_fit.required_bytes.b_bytes ||
      a.matrix_bank_fit.required_bytes.accumulator_bytes !=
          b.matrix_bank_fit.required_bytes.accumulator_bytes ||
      a.matrix_bank_fit.declared_bytes.a_bytes !=
          b.matrix_bank_fit.declared_bytes.a_bytes ||
      a.matrix_bank_fit.declared_bytes.b_bytes !=
          b.matrix_bank_fit.declared_bytes.b_bytes ||
      a.matrix_bank_fit.declared_bytes.accumulator_bytes !=
          b.matrix_bank_fit.declared_bytes.accumulator_bytes ||
      a.matrix_bank_fit.a_fits != b.matrix_bank_fit.a_fits ||
      a.matrix_bank_fit.b_fits != b.matrix_bank_fit.b_fits ||
      a.matrix_bank_fit.accumulator_fits != b.matrix_bank_fit.accumulator_fits ||
      a.matrix_base_estimate.has_value() != b.matrix_base_estimate.has_value() ||
      a.command_counts.has_value() != b.command_counts.has_value() ||
      a.command_issue_cycles.has_value() != b.command_issue_cycles.has_value() ||
      a.combined_matrix_command_compute_cycles !=
          b.combined_matrix_command_compute_cycles ||
      a.rvv_serialized.has_value() != b.rvv_serialized.has_value() ||
      a.matrix_serialized.has_value() != b.matrix_serialized.has_value()) {
    return false;
  }
  if (a.matrix_base_estimate.has_value() &&
      !same_matrix(*a.matrix_base_estimate, *b.matrix_base_estimate)) {
    return false;
  }
  if (a.command_counts.has_value()) {
    const auto& ac = *a.command_counts;
    const auto& bc = *b.command_counts;
    if (ac.clear_accumulator != bc.clear_accumulator || ac.load_a != bc.load_a ||
        ac.load_b != bc.load_b || ac.mac_accumulate != bc.mac_accumulate ||
        ac.store_accumulator_tile != bc.store_accumulator_tile) {
      return false;
    }
  }
  if (a.command_issue_cycles.has_value() &&
      !same_issue(*a.command_issue_cycles, *b.command_issue_cycles)) {
    return false;
  }
  if (a.rvv_serialized.has_value() &&
      !same_serialized(*a.rvv_serialized, *b.rvv_serialized)) {
    return false;
  }
  return !a.matrix_serialized.has_value() ||
         same_serialized(*a.matrix_serialized, *b.matrix_serialized);
}

void test_zero_cost_parity_and_m6_mapping_outputs() {
  const auto zero = MatrixCommandIssueCosts(0, 0, 0, 0, 0);
  const auto input = request(workload(4, 4, 4), 4, 4, 4, 4, 128,
                             32, 32, 64, zero, std::nullopt);
  const auto result = axissim::estimate_matrix_execution(input);
  const auto repeated = axissim::estimate_matrix_execution(input);
  require(same_result(result, repeated),
          "repeated identical requests reproduce every result field");

  require(result.status == MatrixExecutionStatus::complete,
          "exact-fit square fixture completes");
  require_eq(result.rvv_estimate.compute_cycles, 16,
             "M6 RVV cycles are preserved");
  require_eq(result.rvv_estimate.logical_traffic.read_a_bytes, 32,
             "M6 RVV A reads are preserved");
  require_eq(result.rvv_estimate.logical_traffic.read_b_bytes, 128,
             "M6 RVV B reads charge each useful MAC");
  require_eq(result.rvv_estimate.logical_traffic.write_c_bytes, 64,
             "M6 RVV C writes are preserved");
  require_eq(result.rvv_estimate.logical_traffic.total_bytes, 224,
             "M6 RVV total logical traffic is 224 bytes");
  const auto direct_rvv = axissim::estimate_rvv_row_gemm(
      workload(4, 4, 4), axissim::RvvGemmMapping{128, 4, 1, 1, 0, 0});
  require(same_rvv(result.rvv_estimate, direct_rvv),
          "wrapper preserves every M6 RVV estimate field");
  require(result.matrix_base_estimate.has_value(),
          "complete result contains the unchanged M6 matrix estimate");
  const auto& matrix = *result.matrix_base_estimate;
  require_eq(matrix.compute_cycles, 16,
             "M6 tiled-matrix base estimate is hand-derived as 16 cycles");
  require_eq(matrix.tiles_m, 1, "square control has one M tile");
  require_eq(matrix.tiles_n, 1, "square control has one N tile");
  require_eq(matrix.tiles_k, 1, "square control has one K panel");
  require_eq(matrix.working_set_bytes, 128,
             "M6 aggregate working-set equals the 128-byte declared capacity");
  require_eq(matrix.logical_traffic.read_a_bytes, 32,
             "M6 matrix A reads are 32 bytes");
  require_eq(matrix.logical_traffic.read_b_bytes, 32,
             "M6 matrix B reads are 32 bytes");
  require_eq(matrix.logical_traffic.write_c_bytes, 64,
             "M6 matrix C writes are 64 bytes");
  require_eq(matrix.logical_traffic.total_bytes, 128,
             "M6 matrix total logical traffic is 128 bytes");
  const auto direct_matrix = axissim::estimate_tiled_matrix_gemm(
      workload(4, 4, 4), axissim::TiledMatrixGemmMapping{4, 4, 4, 128, 4, 1, 0, 0});
  require(same_matrix(matrix, direct_matrix),
          "wrapper preserves every M6 tiled-matrix estimate field");
  require_eq(result.matrix_bank_fit.required_bytes.a_bytes, 32,
             "one A slot requires 32 bytes");
  require_eq(result.matrix_bank_fit.required_bytes.b_bytes, 32,
             "one B slot requires 32 bytes");
  require_eq(result.matrix_bank_fit.required_bytes.accumulator_bytes, 64,
             "one accumulator slot requires 64 bytes");
  require_eq(result.matrix_bank_fit.required_bytes.a_bytes +
                 result.matrix_bank_fit.required_bytes.b_bytes +
                 result.matrix_bank_fit.required_bytes.accumulator_bytes,
             matrix.working_set_bytes,
             "per-bank demands sum to the M6 aggregate working set");
  require_counts(*result.command_counts, {1, 1, 1, 1, 1},
                 "one-tile one-panel canonical schedule");
  require_eq(result.command_issue_cycles->total, 0,
             "explicit all-zero issue costs add no issue cycles");
  require_eq(*result.combined_matrix_command_compute_cycles, 16,
             "zero-cost command interval equals M6 base cycles");
  require(!result.rvv_serialized && !result.matrix_serialized,
          "explicitly absent serialized sensitivity produces no serial result");
}

void test_bank_fit_distinction_and_capacity_boundaries() {
  const auto zero = MatrixCommandIssueCosts(0, 0, 0, 0, 0);
  const auto split_failure = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 128, 16, 48, 64, zero,
              MatrixSerializedInputs(64'000'000'000ULL, 0)));
  require(split_failure.status == MatrixExecutionStatus::bank_capacity_exceeded,
          "A16/B48/Acc64 fails the A bank while aggregate capacity passes");
  require(split_failure.matrix_base_estimate.has_value(),
          "bank-only failure retains the M6 aggregate estimate for diagnostics");
  require_eq(split_failure.matrix_base_estimate->working_set_bytes, 128,
             "bank-only failure has a passing 128-byte aggregate check");
  require(!split_failure.matrix_bank_fit.a_fits &&
              split_failure.matrix_bank_fit.b_fits &&
              split_failure.matrix_bank_fit.accumulator_fits,
          "only the A bank is under capacity");
  require(split_failure.command_counts.has_value(),
          "bank-only failure retains diagnostic schedule counts");
  require_no_matrix_cycles(split_failure, "bank-only failure");

  const auto one_short = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 128, 31, 33, 64, zero,
              std::nullopt));
  require(one_short.status == MatrixExecutionStatus::bank_capacity_exceeded &&
              !one_short.matrix_bank_fit.a_fits,
          "one-byte-short A bank is rejected despite an unchanged aggregate sum");

  const auto b_one_short = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 128, 32, 31, 65, zero,
              std::nullopt));
  require(b_one_short.status == MatrixExecutionStatus::bank_capacity_exceeded &&
              b_one_short.matrix_bank_fit.a_fits &&
              !b_one_short.matrix_bank_fit.b_fits &&
              b_one_short.matrix_bank_fit.accumulator_fits,
          "one-byte-short B bank is rejected independently");

  const auto accumulator_one_short = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 128, 33, 32, 63, zero,
              std::nullopt));
  require(accumulator_one_short.status ==
                  MatrixExecutionStatus::bank_capacity_exceeded &&
              accumulator_one_short.matrix_bank_fit.a_fits &&
              accumulator_one_short.matrix_bank_fit.b_fits &&
              !accumulator_one_short.matrix_bank_fit.accumulator_fits,
          "one-byte-short accumulator bank is rejected independently");

  const auto aggregate_short = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 127, 32, 32, 63, zero,
              MatrixSerializedInputs(64'000'000'000ULL, 0)));
  require(aggregate_short.status ==
              MatrixExecutionStatus::aggregate_capacity_exceeded,
          "127-byte aggregate fails M6 working-set fit");
  require(aggregate_short.matrix_bank_fit.a_fits &&
              aggregate_short.matrix_bank_fit.b_fits &&
              !aggregate_short.matrix_bank_fit.accumulator_fits,
          "aggregate-underfit result still reports per-bank diagnostics");
  require(!aggregate_short.matrix_base_estimate &&
              !aggregate_short.command_counts,
          "aggregate-underfit result has no nonexistent M6 estimate/counts");
  require_no_matrix_cycles(aggregate_short, "aggregate-underfit result");

  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(request(
            workload(4, 4, 4), 4, 4, 4, 4, 127, 32, 32, 64, zero,
            std::nullopt)));
      },
      "bank sum differing from the explicitly declared aggregate capacity is invalid");
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(request(
            workload(4, 4, 4), 4, 4, 4, 4, 0, 0, 0, 0, zero,
            std::nullopt)));
      },
      "zero aggregate capacity is invalid");
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(request(
            workload(1, 1, 1), 4, 1, 1, 1, 1,
            std::numeric_limits<std::uint64_t>::max(), 1, 1, zero,
            std::nullopt)));
      },
      "bank-capacity sum overflow is rejected before producing a result");
}

void test_issue_cost_breakdown_and_serialized_parity() {
  const auto one_per_command = MatrixCommandIssueCosts(1, 1, 1, 1, 1);
  const auto result = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 128, 32, 32, 64,
              one_per_command,
              MatrixSerializedInputs(64'000'000'000ULL, 0)));

  require(result.status == MatrixExecutionStatus::complete,
          "one-cycle issue fixture completes");
  require_eq(result.matrix_base_estimate->compute_cycles, 16,
             "issue costs do not modify the existing M6 matrix estimate");
  const auto& issue = *result.command_issue_cycles;
  require_eq(issue.clear_accumulator, 1, "clear issue contribution");
  require_eq(issue.load_a, 1, "load A issue contribution");
  require_eq(issue.load_b, 1, "load B issue contribution");
  require_eq(issue.mac_accumulate, 1, "MAC issue contribution");
  require_eq(issue.store_accumulator_tile, 1, "store issue contribution");
  require_eq(issue.total, 5, "five commands add exactly five issue cycles");
  require_eq(*result.combined_matrix_command_compute_cycles, 21,
             "matrix aggregate interval is base 16 plus issue 5 exactly once");

  require(result.rvv_serialized.has_value() &&
              result.matrix_serialized.has_value(),
          "explicit serialized parameters produce paired results");
  const auto& rvv = *result.rvv_serialized;
  require_eq(rvv.read_cycles, 3,
             "RVV A+B 160-byte read rounds up to three cycles at 64 GB/s");
  require_eq(rvv.serialized_engine_service_cycles, 16,
             "RVV serialized engine field is unchanged base compute");
  require_eq(rvv.write_cycles, 1, "RVV 64-byte C write uses one cycle");
  require_eq(rvv.total_cycles, 20,
             "RVV serialized total is read 3 plus engine 16 plus write 1");

  const auto& matrix = *result.matrix_serialized;
  require_eq(matrix.read_cycles, 1,
             "matrix A+B 64-byte read uses one cycle at 64 GB/s");
  require_eq(matrix.serialized_engine_service_cycles, 21,
             "matrix service field includes base plus issue exactly once");
  require_eq(matrix.write_cycles, 1, "matrix C write uses one cycle");
  require_eq(matrix.total_cycles, 23,
             "matrix serial total is read 1 plus engine 21 plus write 1");
  require_eq(result.rvv_estimate.logical_traffic.total_bytes, 224,
             "paired RVV mapping traffic retains M6's per-MAC B reads");
  require_eq(result.matrix_base_estimate->logical_traffic.total_bytes, 128,
             "paired matrix mapping traffic retains its M6 reuse model");
}

void test_irregular_tiles_and_command_order() {
  const auto zero = MatrixCommandIssueCosts(0, 0, 0, 0, 0);
  const auto result = axissim::estimate_matrix_execution(
      request(workload(5, 7, 3), 4, 2, 4, 2, 56, 8, 16, 32, zero,
              std::nullopt));
  require(result.status == MatrixExecutionStatus::complete,
          "irregular rectangular fixture completes");
  const auto& matrix = *result.matrix_base_estimate;
  require_eq(matrix.tiles_m, 3, "irregular M tile count");
  require_eq(matrix.tiles_n, 2, "irregular N tile count");
  require_eq(matrix.tiles_k, 2, "irregular K panel count");
  require_eq(matrix.compute_cycles, 30,
             "M6 active-output tail rule yields 30 P4 cycles");
  require_eq(matrix.working_set_bytes, 56,
             "irregular one-slot aggregate state is 56 bytes");
  require_eq(result.matrix_bank_fit.required_bytes.a_bytes +
                 result.matrix_bank_fit.required_bytes.b_bytes +
                 result.matrix_bank_fit.required_bytes.accumulator_bytes,
             matrix.working_set_bytes,
             "irregular per-bank demands sum to the M6 aggregate working set");
  require_eq(matrix.logical_traffic.read_a_bytes, 60,
             "irregular matrix logical A traffic");
  require_eq(matrix.logical_traffic.read_b_bytes, 126,
             "irregular matrix logical B traffic");
  require_eq(matrix.logical_traffic.write_c_bytes, 140,
             "irregular matrix logical C traffic");
  require_eq(matrix.logical_traffic.total_bytes, 326,
             "irregular matrix logical traffic total");
  require_eq(result.rvv_estimate.compute_cycles, 30,
             "paired irregular RVV mapping has 30 M6 cycles");
  require_eq(result.rvv_estimate.logical_traffic.read_a_bytes, 30,
             "irregular RVV A reads are 30 bytes");
  require_eq(result.rvv_estimate.logical_traffic.read_b_bytes, 210,
             "irregular RVV B reads are charged per useful MAC");
  require_eq(result.rvv_estimate.logical_traffic.write_c_bytes, 140,
             "irregular RVV C writes are 140 bytes");
  require_eq(result.rvv_estimate.logical_traffic.total_bytes, 380,
             "irregular RVV logical traffic total");
  require_counts(*result.command_counts, {6, 12, 12, 12, 6},
                 "irregular 3x2x2 tile schedule");
  require_eq(*result.combined_matrix_command_compute_cycles, 30,
             "zero issue costs preserve the irregular M6 aggregate cycles");

  const auto first_trace = canonical_test_schedule(5, 7, 3, 2, 4, 2);
  require_eq(first_trace.size(), 48,
             "irregular schedule emits the closed-form 48 commands");
  for (std::size_t i = 0; i < first_trace.size(); ++i) {
    require_eq(first_trace[i].sequence_id, i,
               "command sequence identifiers are consecutive");
  }
  require_counts(*result.command_counts, count_test_schedule(first_trace),
                 "estimator counts match independent canonical opcode trace");
  require(first_trace.front().opcode == MatrixOpcode::clear_accumulator &&
              first_trace.front().tile_m_index == 0 &&
              first_trace.front().tile_n_index == 0,
          "canonical command order begins at output tile (0,0)");
  const auto& last = first_trace.back();
  require(last.opcode == MatrixOpcode::store_accumulator_tile &&
              last.tile_m_index == 2 && last.tile_n_index == 1,
          "canonical command order ends after storing final output tile (2,1)");

  std::vector<std::int16_t> a(5 * 3, 0);
  std::vector<std::int16_t> b(3 * 7, 0);
  std::vector<std::int32_t> c(5 * 7, 0);
  axissim::MatrixArchitectureState state({5, 7, 3, 2, 4, 2},
                                         {8, 16, 32}, a, b, c);
  for (const auto& command : first_trace) {
    const auto applied = state.apply(command);
    require(applied.accepted(),
            "canonical aggregate-count trace remains legal in M7.1 state model");
  }
  require(state.complete() && state.next_sequence_id() == first_trace.size(),
          "canonical trace completes M7.1 lifecycle with consecutive IDs");
}

void test_p8_sensitivity_and_fixed_reference_inputs() {
  const auto zero = MatrixCommandIssueCosts(0, 0, 0, 0, 0);
  const auto p4 = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 4, 4, 4, 4, 128, 32, 32, 64, zero,
              std::nullopt));
  const auto p8 = axissim::estimate_matrix_execution(
      request(workload(4, 4, 4), 8, 4, 4, 4, 128, 32, 32, 64, zero,
              std::nullopt));
  require_eq(p4.matrix_base_estimate->compute_cycles, 16,
             "matrix P4 cycle control");
  require_eq(p8.matrix_base_estimate->compute_cycles, 8,
             "matrix P8 cycle control");
  require_eq(p4.rvv_estimate.compute_cycles, 16,
             "RVV P4 row tails take 16 M6 cycles");
  require_eq(p8.rvv_estimate.compute_cycles, 16,
             "RVV P8 retains 16 cycles for VL=4 row tails");
  require_eq(p4.matrix_base_estimate->logical_traffic.total_bytes,
             p8.matrix_base_estimate->logical_traffic.total_bytes,
             "matrix traffic is invariant across P4/P8");
  require_eq(p4.rvv_estimate.logical_traffic.total_bytes,
             p8.rvv_estimate.logical_traffic.total_bytes,
             "RVV traffic is invariant across P4/P8");
  require_counts(*p4.command_counts, {1, 1, 1, 1, 1},
                 "P4 schedule counts");
  require_counts(*p8.command_counts, {1, 1, 1, 1, 1},
                 "P8 schedule counts");

  auto bad_clock = request(workload(4, 4, 4), 4, 4, 4, 4, 128,
                           32, 32, 64, zero, std::nullopt);
  bad_clock.reference_clock_hz = 2'000'000'000ULL;
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(axissim::estimate_matrix_execution(bad_clock)); },
      "unsupported reference clock is rejected");

  auto bad_slot = request(workload(4, 4, 4), 4, 4, 4, 4, 128,
                          32, 32, 64, zero, std::nullopt);
  bad_slot.matrix_mapping.mac_slots_per_cycle = 8;
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(axissim::estimate_matrix_execution(bad_slot)); },
      "mismatched nominal compute slots are rejected");

  const auto zero_rvv_lanes = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 0, 1, 1, 0, 0),
      MatrixMappingInputs(4, 4, 4, 128, 4, 1, 0, 0),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(zero_rvv_lanes));
      },
      "zero RVV lanes are rejected");

  const auto zero_rvv_macs = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 0, 1, 0, 0),
      MatrixMappingInputs(4, 4, 4, 128, 4, 1, 0, 0),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(zero_rvv_macs));
      },
      "zero RVV MACs per lane are rejected");

  const auto zero_matrix_slots = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 0),
      MatrixMappingInputs(4, 4, 4, 128, 0, 1, 0, 0),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(zero_matrix_slots));
      },
      "zero matrix MAC slots are rejected");

  const auto max = std::numeric_limits<std::uint64_t>::max();
  auto overflowing_slots = MatrixExecutionRequest(
      workload(1, 1, 1), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, max, 2, 1, 0, 0),
      MatrixMappingInputs(1, 1, 1, 8, 1, 1, 0, 0),
      MatrixBankCapacityInputs(2, 2, 4), zero, std::nullopt);
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(overflowing_slots));
      },
      "checked RVV slot multiplication rejects overflow");

  const auto bank_demand_overflow = MatrixExecutionRequest(
      workload(1, std::uint64_t{1} << 62, 1), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 0),
      MatrixMappingInputs(1, std::uint64_t{1} << 62, 1, 3, 4, 1, 0, 0),
      MatrixBankCapacityInputs(1, 1, 1), zero, std::nullopt);
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(bank_demand_overflow));
      },
      "checked accumulator-bank byte demand rejects overflow");

  const auto workload_byte_overflow = MatrixExecutionRequest(
      workload(1, std::uint64_t{1} << 62, 1), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 0),
      MatrixMappingInputs(1, 1, 1, 8, 4, 1, 0, 0),
      MatrixBankCapacityInputs(2, 2, 4), zero, std::nullopt);
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(workload_byte_overflow));
      },
      "M6 checked workload byte accounting rejects an unrepresentable C tensor");

  const auto workload_count_overflow = MatrixExecutionRequest(
      workload(1, 1, std::numeric_limits<std::uint64_t>::max()),
      axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 0),
      MatrixMappingInputs(1, 1, 1, 8, 4, 1, 0, 0),
      MatrixBankCapacityInputs(2, 2, 4), zero, std::nullopt);
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(workload_count_overflow));
      },
      "M6 checked conventional operation count rejects overflow");

  const auto bad_bandwidth = request(
      workload(4, 4, 4), 4, 4, 4, 4, 128, 32, 32, 64, zero,
      MatrixSerializedInputs(0, 0));
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(bad_bandwidth));
      },
      "zero serialized bandwidth is rejected");

  const auto nonzero_rvv_setup = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 1, 0),
      MatrixMappingInputs(4, 4, 4, 128, 4, 1, 0, 0),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(nonzero_rvv_setup));
      },
      "nonzero RVV kernel setup is rejected rather than silently modeled");
  const auto nonzero_matrix_setup = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 0),
      MatrixMappingInputs(4, 4, 4, 128, 4, 1, 0, 1),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(nonzero_matrix_setup));
      },
      "nonzero matrix tile setup is rejected rather than silently modeled");
  const auto nonzero_rvv_chunk_setup = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 1),
      MatrixMappingInputs(4, 4, 4, 128, 4, 1, 0, 0),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(nonzero_rvv_chunk_setup));
      },
      "nonzero RVV chunk setup is rejected rather than silently modeled");
  const auto nonzero_matrix_kernel_setup = MatrixExecutionRequest(
      workload(4, 4, 4), axissim::kGemmV1ClockHz,
      RvvMappingInputs(128, 4, 1, 1, 0, 0),
      MatrixMappingInputs(4, 4, 4, 128, 4, 1, 1, 0),
      MatrixBankCapacityInputs(32, 32, 64), zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(nonzero_matrix_kernel_setup));
      },
      "nonzero matrix kernel setup is rejected rather than silently modeled");
}

void test_issue_and_combined_cycle_overflow() {
  const auto max = std::numeric_limits<std::uint64_t>::max();
  const auto issue_total_overflow = request(
      workload(4, 4, 4), 4, 4, 4, 4, 128, 32, 32, 64,
      MatrixCommandIssueCosts(max, max, max, max, max), std::nullopt);
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(issue_total_overflow));
      },
      "itemized issue cycle sum overflow is rejected");

  const auto combined_overflow = request(
      workload(4, 4, 4), 4, 4, 4, 4, 128, 32, 32, 64,
      MatrixCommandIssueCosts(max, 0, 0, 0, 0), std::nullopt);
  require_throws<std::overflow_error>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(combined_overflow));
      },
      "base plus issue cycle overflow is rejected");
}

void test_invalid_workload_and_mappings() {
  const auto zero = MatrixCommandIssueCosts(0, 0, 0, 0, 0);
  auto zero_dimension = request(workload(0, 4, 4), 4, 4, 4, 4, 128,
                                32, 32, 64, zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(axissim::estimate_matrix_execution(zero_dimension)); },
      "zero workload dimension is rejected");

  auto zero_tile = request(workload(4, 4, 4), 4, 0, 4, 4, 128,
                           32, 32, 64, zero, std::nullopt);
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(axissim::estimate_matrix_execution(zero_tile)); },
      "zero matrix tile extent is rejected");

  auto wrong_recurrence = request(workload(4, 4, 4), 4, 4, 4, 4, 128,
                                  32, 32, 64, zero, std::nullopt);
  wrong_recurrence.rvv_mapping.accumulator_recurrence_cycles = 2;
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(wrong_recurrence));
      },
      "unsupported RVV accumulator recurrence is rejected");

  auto wrong_semantics = request(workload(4, 4, 4), 4, 4, 4, 4, 128,
                                 32, 32, 64, zero, std::nullopt);
  wrong_semantics.workload.rounding = GemmRounding::none;
  wrong_semantics.workload.overflow = static_cast<GemmOverflow>(99);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(axissim::estimate_matrix_execution(wrong_semantics));
      },
      "unsupported GEMM semantic enum is rejected");
}

}  // namespace

int main() {
  try {
    test_zero_cost_parity_and_m6_mapping_outputs();
    test_bank_fit_distinction_and_capacity_boundaries();
    test_issue_cost_breakdown_and_serialized_parity();
    test_irregular_tiles_and_command_order();
    test_p8_sensitivity_and_fixed_reference_inputs();
    test_issue_and_combined_cycle_overflow();
    test_invalid_workload_and_mappings();
    std::cout << "M7.2a matrix execution accounting tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "M7.2a matrix execution accounting tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
