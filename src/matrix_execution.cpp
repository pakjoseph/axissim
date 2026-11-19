#include "matrix_execution.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>

namespace axissim {
namespace {

[[nodiscard]] std::uint64_t checked_add(std::uint64_t left,
                                        std::uint64_t right,
                                        const char* what) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::overflow_error(std::string("matrix execution overflow while calculating ") +
                              what);
  }
  return left + right;
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t left,
                                             std::uint64_t right,
                                             const char* what) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::overflow_error(std::string("matrix execution overflow while calculating ") +
                              what);
  }
  return left * right;
}

[[nodiscard]] RvvGemmMapping to_m6_mapping(const RvvMappingInputs& input) {
  return {input.vlen_bits, input.lanes, input.macs_per_lane_cycle,
          input.accumulator_recurrence_cycles, input.kernel_setup_cycles,
          input.chunk_setup_cycles};
}

[[nodiscard]] TiledMatrixGemmMapping to_m6_mapping(
    const MatrixMappingInputs& input) {
  return {input.tile_m, input.tile_n, input.tile_k, input.local_capacity_bytes,
          input.mac_slots_per_cycle, input.accumulator_recurrence_cycles,
          input.kernel_setup_cycles, input.tile_setup_cycles};
}

void validate_request(const MatrixExecutionRequest& request) {
  validate_gemm_workload(request.workload);

  if (request.reference_clock_hz != kGemmV1ClockHz) {
    throw std::invalid_argument(
        "matrix execution reference clock must equal the fixed M6 1 GHz reference");
  }

  const auto& rvv = request.rvv_mapping;
  if (rvv.vlen_bits < 128 || !std::has_single_bit(rvv.vlen_bits) ||
      rvv.lanes == 0 || rvv.macs_per_lane_cycle == 0 ||
      rvv.accumulator_recurrence_cycles != 1 ||
      rvv.kernel_setup_cycles != 0 || rvv.chunk_setup_cycles != 0) {
    throw std::invalid_argument(
        "M7.2a RVV mapping requires valid VLEN/throughput, recurrence one, and zero setup");
  }

  const auto& matrix = request.matrix_mapping;
  if (matrix.tile_m == 0 || matrix.tile_n == 0 || matrix.tile_k == 0 ||
      matrix.local_capacity_bytes == 0 || matrix.mac_slots_per_cycle == 0 ||
      matrix.accumulator_recurrence_cycles != 1 ||
      matrix.kernel_setup_cycles != 0 || matrix.tile_setup_cycles != 0) {
    throw std::invalid_argument(
        "M7.2a matrix mapping requires positive geometry/capacity/throughput, recurrence one, and zero setup");
  }

  const std::uint64_t rvv_slots = checked_multiply(
      rvv.lanes, rvv.macs_per_lane_cycle, "RVV MAC slots per cycle");
  if (rvv_slots != matrix.mac_slots_per_cycle) {
    throw std::invalid_argument(
        "RVV and matrix mappings must use equal nominal MAC slots per cycle");
  }

  if (request.serialized_parameters.has_value() &&
      request.serialized_parameters->bandwidth_bytes_per_second == 0) {
    throw std::invalid_argument("serialized backing-store bandwidth must be positive");
  }
}

[[nodiscard]] MatrixBankCapacities to_m7_capacities(
    const MatrixBankCapacityInputs& inputs) {
  return {inputs.a_bytes, inputs.b_bytes, inputs.accumulator_bytes};
}

[[nodiscard]] MatrixBankCapacities required_bank_bytes(
    const GemmWorkload& workload, const MatrixMappingInputs& mapping) {
  const auto max_m = std::min(workload.m, mapping.tile_m);
  const auto max_n = std::min(workload.n, mapping.tile_n);
  const auto max_k = std::min(workload.k, mapping.tile_k);
  const auto a_elements = checked_multiply(max_m, max_k, "A bank elements");
  const auto b_elements = checked_multiply(max_k, max_n, "B bank elements");
  const auto accumulator_elements = checked_multiply(max_m, max_n,
                                                      "accumulator bank elements");
  return {checked_multiply(a_elements, 2, "A bank bytes"),
          checked_multiply(b_elements, 2, "B bank bytes"),
          checked_multiply(accumulator_elements, 4, "accumulator bank bytes")};
}

[[nodiscard]] MatrixBankFit make_bank_fit(const MatrixBankCapacities& required,
                                           const MatrixBankCapacities& declared) {
  return {required, declared,
          declared.a_bytes >= required.a_bytes,
          declared.b_bytes >= required.b_bytes,
          declared.accumulator_bytes >= required.accumulator_bytes};
}

[[nodiscard]] MatrixCommandCounts command_counts(
    const TiledMatrixGemmEstimate& estimate) {
  const auto output_tiles = checked_multiply(estimate.tiles_m, estimate.tiles_n,
                                             "output tile count");
  const auto tile_panels = checked_multiply(output_tiles, estimate.tiles_k,
                                            "tile-panel command count");
  return {output_tiles, tile_panels, tile_panels, tile_panels, output_tiles};
}

[[nodiscard]] MatrixCommandIssueCycles issue_cycles(
    const MatrixCommandCounts& counts, const MatrixCommandIssueCosts& costs) {
  const auto clear = checked_multiply(
      counts.clear_accumulator, costs.clear_accumulator_cycles_per_command,
      "clear command issue cycles");
  const auto load_a = checked_multiply(
      counts.load_a, costs.load_a_cycles_per_command,
      "load A command issue cycles");
  const auto load_b = checked_multiply(
      counts.load_b, costs.load_b_cycles_per_command,
      "load B command issue cycles");
  const auto accumulate = checked_multiply(
      counts.mac_accumulate, costs.mac_accumulate_cycles_per_command,
      "MAC command issue cycles");
  const auto store = checked_multiply(
      counts.store_accumulator_tile,
      costs.store_accumulator_tile_cycles_per_command,
      "store command issue cycles");
  auto total = checked_add(clear, load_a, "total command issue cycles");
  total = checked_add(total, load_b, "total command issue cycles");
  total = checked_add(total, accumulate, "total command issue cycles");
  total = checked_add(total, store, "total command issue cycles");
  return {clear, load_a, load_b, accumulate, store, total};
}

[[nodiscard]] MatrixSerializedCycles serialized_cycles(
    const GemmLogicalTraffic& traffic, std::uint64_t engine_cycles,
    const MatrixSerializedInputs& inputs) {
  const SerializedBackingStoreParameters m6_parameters{
      inputs.bandwidth_bytes_per_second, inputs.batch_setup_cycles};
  const auto estimate = estimate_serialized_backing_store(
      traffic, engine_cycles, m6_parameters);
  return {estimate.read_cycles, estimate.compute_cycles, estimate.write_cycles,
          estimate.total_cycles};
}

[[nodiscard]] bool fits_all_banks(const MatrixBankFit& fit) noexcept {
  return fit.a_fits && fit.b_fits && fit.accumulator_fits;
}

}  // namespace

RvvMappingInputs::RvvMappingInputs(std::uint64_t vlen,
                                   std::uint64_t lane_count,
                                   std::uint64_t macs_per_lane,
                                   std::uint64_t recurrence,
                                   std::uint64_t kernel_setup,
                                   std::uint64_t chunk_setup)
    : vlen_bits(vlen),
      lanes(lane_count),
      macs_per_lane_cycle(macs_per_lane),
      accumulator_recurrence_cycles(recurrence),
      kernel_setup_cycles(kernel_setup),
      chunk_setup_cycles(chunk_setup) {}

MatrixMappingInputs::MatrixMappingInputs(
    std::uint64_t tm, std::uint64_t tn, std::uint64_t tk,
    std::uint64_t local_capacity, std::uint64_t mac_slots,
    std::uint64_t recurrence, std::uint64_t kernel_setup,
    std::uint64_t tile_setup)
    : tile_m(tm),
      tile_n(tn),
      tile_k(tk),
      local_capacity_bytes(local_capacity),
      mac_slots_per_cycle(mac_slots),
      accumulator_recurrence_cycles(recurrence),
      kernel_setup_cycles(kernel_setup),
      tile_setup_cycles(tile_setup) {}

MatrixBankCapacityInputs::MatrixBankCapacityInputs(
    std::uint64_t a, std::uint64_t b, std::uint64_t accumulator)
    : a_bytes(a), b_bytes(b), accumulator_bytes(accumulator) {}

MatrixCommandIssueCosts::MatrixCommandIssueCosts(
    std::uint64_t clear, std::uint64_t load_a, std::uint64_t load_b,
    std::uint64_t accumulate, std::uint64_t store)
    : clear_accumulator_cycles_per_command(clear),
      load_a_cycles_per_command(load_a),
      load_b_cycles_per_command(load_b),
      mac_accumulate_cycles_per_command(accumulate),
      store_accumulator_tile_cycles_per_command(store) {}

MatrixSerializedInputs::MatrixSerializedInputs(std::uint64_t bandwidth,
                                               std::uint64_t batch_setup)
    : bandwidth_bytes_per_second(bandwidth),
      batch_setup_cycles(batch_setup) {}

MatrixExecutionRequest::MatrixExecutionRequest(
    GemmWorkload input_workload, std::uint64_t input_reference_clock_hz,
    RvvMappingInputs input_rvv_mapping,
    MatrixMappingInputs input_matrix_mapping,
    MatrixBankCapacityInputs input_bank_capacities,
    MatrixCommandIssueCosts input_command_issue_costs,
    std::optional<MatrixSerializedInputs> input_serialized_parameters)
    : workload(input_workload),
      reference_clock_hz(input_reference_clock_hz),
      rvv_mapping(input_rvv_mapping),
      matrix_mapping(input_matrix_mapping),
      matrix_bank_capacities(input_bank_capacities),
      command_issue_costs(input_command_issue_costs),
      serialized_parameters(input_serialized_parameters) {}

MatrixExecutionResult estimate_matrix_execution(
    const MatrixExecutionRequest& request) {
  validate_request(request);

  const auto declared = to_m7_capacities(request.matrix_bank_capacities);
  auto declared_sum = checked_add(declared.a_bytes, declared.b_bytes,
                                  "declared bank capacity sum");
  declared_sum = checked_add(declared_sum, declared.accumulator_bytes,
                             "declared bank capacity sum");
  if (declared_sum == 0 ||
      declared_sum != request.matrix_mapping.local_capacity_bytes) {
    throw std::invalid_argument(
        "matrix bank capacities must sum to the declared aggregate local capacity");
  }

  const auto required = required_bank_bytes(request.workload,
                                             request.matrix_mapping);
  const auto fit = make_bank_fit(required, declared);
  const auto rvv_mapping = to_m6_mapping(request.rvv_mapping);
  const auto matrix_mapping = to_m6_mapping(request.matrix_mapping);
  const auto rvv_estimate = estimate_rvv_row_gemm(request.workload, rvv_mapping);

  MatrixExecutionResult result{
      MatrixExecutionStatus::complete,
      request,
      rvv_estimate,
      fit,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};

  TiledMatrixGemmEstimate matrix_estimate{};
  try {
    matrix_estimate = estimate_tiled_matrix_gemm(request.workload,
                                                  matrix_mapping);
  } catch (const std::length_error&) {
    result.status = MatrixExecutionStatus::aggregate_capacity_exceeded;
    return result;
  }

  result.matrix_base_estimate = matrix_estimate;
  result.command_counts = command_counts(matrix_estimate);
  if (!fits_all_banks(fit)) {
    result.status = MatrixExecutionStatus::bank_capacity_exceeded;
    return result;
  }

  const auto issue = issue_cycles(*result.command_counts,
                                  request.command_issue_costs);
  const auto combined = checked_add(matrix_estimate.compute_cycles, issue.total,
                                    "matrix command/compute interval");
  result.command_issue_cycles = issue;
  result.combined_matrix_command_compute_cycles = combined;

  if (request.serialized_parameters.has_value()) {
    const auto& serialized = *request.serialized_parameters;
    result.rvv_serialized = serialized_cycles(
        rvv_estimate.logical_traffic, rvv_estimate.compute_cycles, serialized);
    result.matrix_serialized = serialized_cycles(
        matrix_estimate.logical_traffic, combined, serialized);
  }

  return result;
}

}  // namespace axissim
