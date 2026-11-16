#pragma once

#include <cstdint>

#include "gemm_workload.hpp"

namespace axissim {

inline constexpr std::uint64_t kGemmV1ClockHz = 1'000'000'000;
inline constexpr std::uint64_t kGemmV1ElementBits = 16;

struct GemmLogicalTraffic {
  std::uint64_t read_a_bytes = 0;
  std::uint64_t read_b_bytes = 0;
  std::uint64_t write_c_bytes = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t read_a_batches = 0;
  std::uint64_t read_b_batches = 0;
  std::uint64_t write_c_batches = 0;
};

struct RvvGemmMapping {
  std::uint64_t vlen_bits = 128;
  std::uint64_t lanes = 4;
  std::uint64_t macs_per_lane_cycle = 1;
  std::uint64_t accumulator_recurrence_cycles = 1;
  std::uint64_t kernel_setup_cycles = 0;
  std::uint64_t chunk_setup_cycles = 0;
};

struct RvvGemmEstimate {
  GemmWorkCounts work;
  GemmLogicalTraffic logical_traffic;
  std::uint64_t vlmax = 0;
  std::uint64_t mac_slots_per_cycle = 0;
  std::uint64_t vector_mac_instructions = 0;
  std::uint64_t compute_cycles = 0;
};

struct TiledMatrixGemmMapping {
  std::uint64_t tile_m = 2;
  std::uint64_t tile_n = 2;
  std::uint64_t tile_k = 2;
  std::uint64_t local_capacity_bytes = 32;
  std::uint64_t mac_slots_per_cycle = 4;
  std::uint64_t accumulator_recurrence_cycles = 1;
  std::uint64_t kernel_setup_cycles = 0;
  std::uint64_t tile_setup_cycles = 0;
};

struct TiledMatrixGemmEstimate {
  GemmWorkCounts work;
  GemmLogicalTraffic logical_traffic;
  std::uint64_t tiles_m = 0;
  std::uint64_t tiles_n = 0;
  std::uint64_t tiles_k = 0;
  std::uint64_t matrix_tile_operations = 0;
  std::uint64_t working_set_bytes = 0;
  std::uint64_t local_capacity_bytes = 0;
  std::uint64_t mac_slots_per_cycle = 0;
  std::uint64_t compute_cycles = 0;
};

struct SerializedBackingStoreParameters {
  std::uint64_t bandwidth_bytes_per_second = 0;
  std::uint64_t batch_setup_cycles = 0;
};

struct SerializedBackingStoreEstimate {
  std::uint64_t read_cycles = 0;
  std::uint64_t compute_cycles = 0;
  std::uint64_t write_cycles = 0;
  std::uint64_t total_cycles = 0;
};

// Analytical RVV row/column-vector map. It estimates a defined schedule and
// logical payload only; it does not execute GEMM values or model RVV timing at
// instruction or silicon level. Throws std::invalid_argument for invalid
// mapping/workload semantics and std::overflow_error for unrepresentable counts.
[[nodiscard]] RvvGemmEstimate estimate_rvv_row_gemm(
    const GemmWorkload& workload, const RvvGemmMapping& mapping);

// Generic output-stationary tiled matrix map. The local capacity is a fit
// constraint, not an SRAM energy or physical storage model. Throws
// std::invalid_argument for invalid mapping/workload semantics,
// std::overflow_error for unrepresentable counts, and std::length_error if the
// declared working set exceeds local capacity.
[[nodiscard]] TiledMatrixGemmEstimate estimate_tiled_matrix_gemm(
    const GemmWorkload& workload, const TiledMatrixGemmMapping& mapping);

// Optional deterministic serialized-bandwidth sensitivity. Read A/B, compute,
// then write C; no overlap, queueing, DRAM command behavior, or energy is
// modeled. This does not change the compute-only estimates above. Throws
// std::invalid_argument for zero bandwidth or inconsistent traffic totals and
// std::overflow_error for unrepresentable cycle arithmetic.
[[nodiscard]] SerializedBackingStoreEstimate estimate_serialized_backing_store(
    const GemmLogicalTraffic& traffic, std::uint64_t compute_cycles,
    const SerializedBackingStoreParameters& parameters);

}  // namespace axissim
