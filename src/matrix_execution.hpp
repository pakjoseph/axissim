#pragma once

#include <cstdint>
#include <optional>

#include "gemm_mapping.hpp"
#include "matrix_architecture.hpp"

namespace axissim {

// M7.2a request records require explicit values. The synthetic capacities,
// setup costs, and issue costs used by tests are accounting fixtures, not
// hardware characterization.
struct RvvMappingInputs {
  std::uint64_t vlen_bits;
  std::uint64_t lanes;
  std::uint64_t macs_per_lane_cycle;
  std::uint64_t accumulator_recurrence_cycles;
  std::uint64_t kernel_setup_cycles;
  std::uint64_t chunk_setup_cycles;

  RvvMappingInputs(std::uint64_t vlen, std::uint64_t lane_count,
                   std::uint64_t macs_per_lane, std::uint64_t recurrence,
                   std::uint64_t kernel_setup, std::uint64_t chunk_setup);
  RvvMappingInputs() = delete;
};

struct MatrixMappingInputs {
  std::uint64_t tile_m;
  std::uint64_t tile_n;
  std::uint64_t tile_k;
  std::uint64_t local_capacity_bytes;
  std::uint64_t mac_slots_per_cycle;
  std::uint64_t accumulator_recurrence_cycles;
  std::uint64_t kernel_setup_cycles;
  std::uint64_t tile_setup_cycles;

  MatrixMappingInputs(std::uint64_t tm, std::uint64_t tn, std::uint64_t tk,
                      std::uint64_t local_capacity, std::uint64_t mac_slots,
                      std::uint64_t recurrence, std::uint64_t kernel_setup,
                      std::uint64_t tile_setup);
  MatrixMappingInputs() = delete;
};

struct MatrixBankCapacityInputs {
  std::uint64_t a_bytes;
  std::uint64_t b_bytes;
  std::uint64_t accumulator_bytes;

  MatrixBankCapacityInputs(std::uint64_t a, std::uint64_t b,
                           std::uint64_t accumulator);
  MatrixBankCapacityInputs() = delete;
};

struct MatrixCommandIssueCosts {
  std::uint64_t clear_accumulator_cycles_per_command;
  std::uint64_t load_a_cycles_per_command;
  std::uint64_t load_b_cycles_per_command;
  std::uint64_t mac_accumulate_cycles_per_command;
  std::uint64_t store_accumulator_tile_cycles_per_command;

  MatrixCommandIssueCosts(std::uint64_t clear, std::uint64_t load_a,
                          std::uint64_t load_b, std::uint64_t accumulate,
                          std::uint64_t store);
  MatrixCommandIssueCosts() = delete;
};

struct MatrixSerializedInputs {
  std::uint64_t bandwidth_bytes_per_second;
  std::uint64_t batch_setup_cycles;

  MatrixSerializedInputs(std::uint64_t bandwidth, std::uint64_t batch_setup);
  MatrixSerializedInputs() = delete;
};

struct MatrixExecutionRequest {
  GemmWorkload workload;
  std::uint64_t reference_clock_hz;
  RvvMappingInputs rvv_mapping;
  MatrixMappingInputs matrix_mapping;
  MatrixBankCapacityInputs matrix_bank_capacities;
  MatrixCommandIssueCosts command_issue_costs;
  std::optional<MatrixSerializedInputs> serialized_parameters;

  MatrixExecutionRequest(
      GemmWorkload workload, std::uint64_t reference_clock_hz,
      RvvMappingInputs rvv_mapping, MatrixMappingInputs matrix_mapping,
      MatrixBankCapacityInputs matrix_bank_capacities,
      MatrixCommandIssueCosts command_issue_costs,
      std::optional<MatrixSerializedInputs> serialized_parameters);
  MatrixExecutionRequest() = delete;
};

enum class MatrixExecutionStatus : std::uint8_t {
  complete,
  aggregate_capacity_exceeded,
  bank_capacity_exceeded,
};

struct MatrixBankFit {
  MatrixBankCapacities required_bytes;
  MatrixBankCapacities declared_bytes;
  bool a_fits;
  bool b_fits;
  bool accumulator_fits;
};

struct MatrixCommandCounts {
  std::uint64_t clear_accumulator;
  std::uint64_t load_a;
  std::uint64_t load_b;
  std::uint64_t mac_accumulate;
  std::uint64_t store_accumulator_tile;
};

struct MatrixCommandIssueCycles {
  std::uint64_t clear_accumulator;
  std::uint64_t load_a;
  std::uint64_t load_b;
  std::uint64_t mac_accumulate;
  std::uint64_t store_accumulator_tile;
  std::uint64_t total;
};

struct MatrixSerializedCycles {
  std::uint64_t read_cycles;
  std::uint64_t serialized_engine_service_cycles;
  std::uint64_t write_cycles;
  std::uint64_t total_cycles;
};

struct MatrixExecutionResult {
  MatrixExecutionStatus status;
  MatrixExecutionRequest evaluated_request;
  RvvGemmEstimate rvv_estimate;
  MatrixBankFit matrix_bank_fit;
  std::optional<TiledMatrixGemmEstimate> matrix_base_estimate;
  std::optional<MatrixCommandCounts> command_counts;
  std::optional<MatrixCommandIssueCycles> command_issue_cycles;
  // Aggregate command/compute interval for this request's single GEMM work unit;
  // no per-command or per-tile timing is inferred.
  std::optional<std::uint64_t> combined_matrix_command_compute_cycles;
  std::optional<MatrixSerializedCycles> rvv_serialized;
  std::optional<MatrixSerializedCycles> matrix_serialized;
};

[[nodiscard]] MatrixExecutionResult estimate_matrix_execution(
    const MatrixExecutionRequest& request);

}  // namespace axissim
