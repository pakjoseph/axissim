#include "gemm_mapping.hpp"

#include <algorithm>
#include <array>
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
    throw std::overflow_error(std::string("GEMM overflow while calculating ") + what);
  }
  return left + right;
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t left,
                                             std::uint64_t right,
                                             const char* what) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::overflow_error(std::string("GEMM overflow while calculating ") + what);
  }
  return left * right;
}

[[nodiscard]] std::uint64_t ceil_div(std::uint64_t numerator,
                                     std::uint64_t denominator) {
  if (denominator == 0) throw std::invalid_argument("GEMM divisor must be positive");
  return numerator / denominator + static_cast<std::uint64_t>(numerator % denominator != 0);
}

[[nodiscard]] std::uint64_t checked_total_bytes(const GemmLogicalTraffic& traffic) {
  const std::uint64_t read_bytes = checked_add(traffic.read_a_bytes,
                                               traffic.read_b_bytes,
                                               "read byte total");
  return checked_add(read_bytes, traffic.write_c_bytes, "logical byte total");
}

void validate_transfer_batch_pair(std::uint64_t bytes, std::uint64_t batches,
                                  const char* transfer_name) {
  if ((bytes == 0) != (batches == 0)) {
    throw std::invalid_argument(std::string("GEMM ") + transfer_name +
                                " bytes and batch count must both be zero or both be nonzero");
  }
}

[[nodiscard]] GemmLogicalTraffic make_traffic(std::uint64_t read_a_bytes,
                                              std::uint64_t read_b_bytes,
                                              std::uint64_t write_c_bytes,
                                              std::uint64_t read_a_batches,
                                              std::uint64_t read_b_batches,
                                              std::uint64_t write_c_batches) {
  GemmLogicalTraffic traffic;
  traffic.read_a_bytes = read_a_bytes;
  traffic.read_b_bytes = read_b_bytes;
  traffic.write_c_bytes = write_c_bytes;
  traffic.total_bytes = checked_total_bytes(traffic);
  traffic.read_a_batches = read_a_batches;
  traffic.read_b_batches = read_b_batches;
  traffic.write_c_batches = write_c_batches;
  return traffic;
}

void validate_rvv_mapping(const RvvGemmMapping& mapping) {
  if (mapping.vlen_bits < 128 || !std::has_single_bit(mapping.vlen_bits)) {
    throw std::invalid_argument("RVV v1 VLEN must be a power of two of at least 128 bits");
  }
  if (mapping.lanes == 0 || mapping.macs_per_lane_cycle == 0 ||
      mapping.accumulator_recurrence_cycles == 0) {
    throw std::invalid_argument("RVV lanes, MACs per lane, and recurrence must be positive");
  }
}

void validate_matrix_mapping(const TiledMatrixGemmMapping& mapping) {
  if (mapping.tile_m == 0 || mapping.tile_n == 0 || mapping.tile_k == 0 ||
      mapping.local_capacity_bytes == 0 || mapping.mac_slots_per_cycle == 0 ||
      mapping.accumulator_recurrence_cycles == 0) {
    throw std::invalid_argument("matrix tile dimensions, capacity, MAC slots, and recurrence must be positive");
  }
}

struct ExtentMultiplicity {
  std::uint64_t extent = 0;
  std::uint64_t count = 0;
};

struct TileExtents {
  std::array<ExtentMultiplicity, 2> groups{};
  std::size_t group_count = 0;
  std::uint64_t tile_count = 0;
};

[[nodiscard]] TileExtents split_tile_extents(std::uint64_t dimension,
                                             std::uint64_t tile_extent) {
  TileExtents result;
  const std::uint64_t full_count = dimension / tile_extent;
  const std::uint64_t tail_extent = dimension % tile_extent;
  if (full_count > 0) {
    result.groups[result.group_count++] = {tile_extent, full_count};
  }
  if (tail_extent > 0) {
    result.groups[result.group_count++] = {tail_extent, 1};
  }
  result.tile_count = checked_add(full_count,
                                  static_cast<std::uint64_t>(tail_extent > 0),
                                  "tile count");
  return result;
}

[[nodiscard]] std::uint64_t add_scaled(std::uint64_t total,
                                       std::uint64_t value,
                                       std::uint64_t multiplicity,
                                       const char* what) {
  return checked_add(total, checked_multiply(value, multiplicity, what), what);
}

}  // namespace

void validate_gemm_workload(const GemmWorkload& workload) {
  if (workload.m == 0 || workload.n == 0 || workload.k == 0) {
    throw std::invalid_argument("GEMM M, N, and K dimensions must be positive");
  }
  if (workload.input_format != GemmInputFormat::signed_i16_twos_complement ||
      workload.accumulator_output_format !=
          GemmAccumulatorOutputFormat::signed_i32_twos_complement ||
      workload.layout != GemmLayout::row_major_contiguous ||
      workload.overflow != GemmOverflow::wrap_modulo_2pow32 ||
      workload.rounding != GemmRounding::none ||
      workload.initial_c != GemmInitialC::zero) {
    throw std::invalid_argument("unsupported GEMM v1 numeric or layout semantics");
  }
}

GemmWorkCounts count_gemm_work(const GemmWorkload& workload) {
  validate_gemm_workload(workload);
  const std::uint64_t mn = checked_multiply(workload.m, workload.n, "M*N");
  const std::uint64_t useful_macs = checked_multiply(mn, workload.k, "M*N*K");
  const std::uint64_t conventional_ops = checked_multiply(useful_macs, 2,
                                                          "conventional operation equivalents");
  const std::uint64_t tensor_a_elements = checked_multiply(workload.m, workload.k,
                                                           "A element count");
  const std::uint64_t tensor_b_elements = checked_multiply(workload.k, workload.n,
                                                           "B element count");
  const std::uint64_t tensor_a_bytes = checked_multiply(tensor_a_elements, 2,
                                                        "A logical bytes");
  const std::uint64_t tensor_b_bytes = checked_multiply(tensor_b_elements, 2,
                                                        "B logical bytes");
  const std::uint64_t tensor_c_bytes = checked_multiply(mn, 4, "C logical bytes");
  const std::uint64_t ab_bytes = checked_add(tensor_a_bytes, tensor_b_bytes,
                                             "unique A+B payload bytes");
  const std::uint64_t unique_bytes = checked_add(ab_bytes, tensor_c_bytes,
                                                 "unique tensor payload bytes");
  return {useful_macs, conventional_ops, tensor_a_bytes, tensor_b_bytes,
          tensor_c_bytes, unique_bytes};
}

RvvGemmEstimate estimate_rvv_row_gemm(const GemmWorkload& workload,
                                      const RvvGemmMapping& mapping) {
  validate_rvv_mapping(mapping);
  const GemmWorkCounts work = count_gemm_work(workload);
  const std::uint64_t vlmax = mapping.vlen_bits / kGemmV1ElementBits;
  const std::uint64_t mac_slots = checked_multiply(mapping.lanes,
                                                   mapping.macs_per_lane_cycle,
                                                   "RVV MAC slots per cycle");
  const std::uint64_t full_chunks_per_row = workload.n / vlmax;
  const std::uint64_t tail_vl = workload.n % vlmax;
  const std::uint64_t chunks_per_row = checked_add(
      full_chunks_per_row, static_cast<std::uint64_t>(tail_vl > 0),
      "RVV chunks per row");

  const auto chunk_cycles = [&](std::uint64_t vl) {
    const std::uint64_t active_cycles = ceil_div(vl, mac_slots);
    const std::uint64_t per_k_cycles = std::max(active_cycles,
                                                mapping.accumulator_recurrence_cycles);
    const std::uint64_t compute_cycles = checked_multiply(workload.k, per_k_cycles,
                                                          "RVV chunk compute cycles");
    return checked_add(mapping.chunk_setup_cycles, compute_cycles,
                       "RVV chunk service cycles");
  };

  std::uint64_t total_cycles = mapping.kernel_setup_cycles;
  const std::uint64_t full_chunks = checked_multiply(workload.m, full_chunks_per_row,
                                                      "RVV full chunk count");
  if (full_chunks > 0) {
    total_cycles = add_scaled(total_cycles, chunk_cycles(vlmax), full_chunks,
                              "RVV full chunk cycles");
  }
  if (tail_vl > 0) {
    total_cycles = add_scaled(total_cycles, chunk_cycles(tail_vl), workload.m,
                              "RVV tail chunk cycles");
  }

  const std::uint64_t row_chunk_count = checked_multiply(workload.m,
                                                        chunks_per_row,
                                                        "RVV row-chunk count");
  const std::uint64_t row_chunk_k_count = checked_multiply(row_chunk_count,
                                                           workload.k,
                                                           "RVV row-chunk-K count");
  const std::uint64_t vector_mac_instructions = row_chunk_k_count;
  const std::uint64_t read_a_bytes = checked_multiply(row_chunk_k_count, 2,
                                                       "RVV A logical bytes");
  const std::uint64_t read_b_bytes = checked_multiply(work.useful_macs, 2,
                                                       "RVV B logical bytes");
  const std::uint64_t read_batches = row_chunk_k_count;
  const GemmLogicalTraffic traffic = make_traffic(
      read_a_bytes, read_b_bytes, work.tensor_c_bytes, read_batches,
      read_batches, row_chunk_count);

  return {work, traffic, vlmax, mac_slots, vector_mac_instructions, total_cycles};
}

TiledMatrixGemmEstimate estimate_tiled_matrix_gemm(
    const GemmWorkload& workload, const TiledMatrixGemmMapping& mapping) {
  validate_matrix_mapping(mapping);
  const GemmWorkCounts work = count_gemm_work(workload);
  const TileExtents m_extents = split_tile_extents(workload.m, mapping.tile_m);
  const TileExtents n_extents = split_tile_extents(workload.n, mapping.tile_n);
  const TileExtents k_extents = split_tile_extents(workload.k, mapping.tile_k);
  const std::uint64_t mn_tiles = checked_multiply(m_extents.tile_count,
                                                  n_extents.tile_count,
                                                  "output tile count");
  const std::uint64_t tile_operations = checked_multiply(mn_tiles,
                                                          k_extents.tile_count,
                                                          "matrix tile operation count");

  const std::uint64_t max_m = std::min(workload.m, mapping.tile_m);
  const std::uint64_t max_n = std::min(workload.n, mapping.tile_n);
  const std::uint64_t max_k = std::min(workload.k, mapping.tile_k);
  const std::uint64_t working_a = checked_multiply(
      checked_multiply(max_m, max_k, "matrix A working elements"), 2,
      "matrix A working bytes");
  const std::uint64_t working_b = checked_multiply(
      checked_multiply(max_k, max_n, "matrix B working elements"), 2,
      "matrix B working bytes");
  const std::uint64_t working_c = checked_multiply(
      checked_multiply(max_m, max_n, "matrix C working elements"), 4,
      "matrix C working bytes");
  const std::uint64_t working_ab = checked_add(working_a, working_b,
                                               "matrix A+B working bytes");
  const std::uint64_t working_set = checked_add(working_ab, working_c,
                                                "matrix working set");
  if (working_set > mapping.local_capacity_bytes) {
    throw std::length_error("matrix working set " + std::to_string(working_set) +
                            " bytes exceeds local capacity " +
                            std::to_string(mapping.local_capacity_bytes) + " bytes");
  }

  std::uint64_t compute_cycles = mapping.kernel_setup_cycles;
  for (std::size_t m_index = 0; m_index < m_extents.group_count; ++m_index) {
    const auto m_group = m_extents.groups[m_index];
    for (std::size_t n_index = 0; n_index < n_extents.group_count; ++n_index) {
      const auto n_group = n_extents.groups[n_index];
      const std::uint64_t active_outputs = checked_multiply(m_group.extent,
                                                            n_group.extent,
                                                            "matrix tile outputs");
      const std::uint64_t cycles_per_mac_step = std::max(
          ceil_div(active_outputs, mapping.mac_slots_per_cycle),
          mapping.accumulator_recurrence_cycles);
      for (std::size_t k_index = 0; k_index < k_extents.group_count; ++k_index) {
        const auto k_group = k_extents.groups[k_index];
        const std::uint64_t active_tile_cycles = checked_multiply(
            k_group.extent, cycles_per_mac_step, "matrix K-tile compute cycles");
        const std::uint64_t cycles_per_tile = checked_add(
            active_tile_cycles, mapping.tile_setup_cycles,
            "matrix tile service cycles");
        const std::uint64_t multiplicity_mn = checked_multiply(
            m_group.count, n_group.count, "matrix output tile multiplicity");
        const std::uint64_t multiplicity = checked_multiply(
            multiplicity_mn, k_group.count, "matrix tile multiplicity");
        compute_cycles = add_scaled(compute_cycles, cycles_per_tile, multiplicity,
                                    "matrix compute cycles");
      }
    }
  }

  const std::uint64_t a_tile_elements = checked_multiply(
      checked_multiply(workload.m, n_extents.tile_count, "matrix A tile reuse"),
      workload.k, "matrix A read elements");
  const std::uint64_t b_tile_elements = checked_multiply(
      checked_multiply(workload.n, m_extents.tile_count, "matrix B tile reuse"),
      workload.k, "matrix B read elements");
  const std::uint64_t read_a_bytes = checked_multiply(a_tile_elements, 2,
                                                       "matrix A logical bytes");
  const std::uint64_t read_b_bytes = checked_multiply(b_tile_elements, 2,
                                                       "matrix B logical bytes");
  const GemmLogicalTraffic traffic = make_traffic(
      read_a_bytes, read_b_bytes, work.tensor_c_bytes, tile_operations,
      tile_operations, mn_tiles);

  return {work, traffic, m_extents.tile_count, n_extents.tile_count,
          k_extents.tile_count, tile_operations, working_set,
          mapping.local_capacity_bytes, mapping.mac_slots_per_cycle,
          compute_cycles};
}

SerializedBackingStoreEstimate estimate_serialized_backing_store(
    const GemmLogicalTraffic& traffic, std::uint64_t compute_cycles,
    const SerializedBackingStoreParameters& parameters) {
  if (parameters.bandwidth_bytes_per_second == 0) {
    throw std::invalid_argument("serialized backing-store bandwidth must be positive");
  }
  if (traffic.total_bytes != checked_total_bytes(traffic)) {
    throw std::invalid_argument("GEMM logical traffic total does not match its A/B/C components");
  }
  validate_transfer_batch_pair(traffic.read_a_bytes, traffic.read_a_batches,
                               "A read");
  validate_transfer_batch_pair(traffic.read_b_bytes, traffic.read_b_batches,
                               "B read");
  validate_transfer_batch_pair(traffic.write_c_bytes, traffic.write_c_batches,
                               "C write");
  const std::uint64_t read_bytes = checked_add(traffic.read_a_bytes,
                                               traffic.read_b_bytes,
                                               "serialized read bytes");
  const std::uint64_t read_batches = checked_add(traffic.read_a_batches,
                                                 traffic.read_b_batches,
                                                 "serialized read batch count");
  const std::uint64_t read_numerator = checked_multiply(
      read_bytes, kGemmV1ClockHz, "serialized read bandwidth cycles");
  const std::uint64_t write_numerator = checked_multiply(
      traffic.write_c_bytes, kGemmV1ClockHz, "serialized write bandwidth cycles");
  const std::uint64_t read_transfer_cycles = ceil_div(
      read_numerator, parameters.bandwidth_bytes_per_second);
  const std::uint64_t write_transfer_cycles = ceil_div(
      write_numerator, parameters.bandwidth_bytes_per_second);
  const std::uint64_t read_setup_cycles = checked_multiply(
      read_batches, parameters.batch_setup_cycles, "serialized read setup cycles");
  const std::uint64_t write_setup_cycles = checked_multiply(
      traffic.write_c_batches, parameters.batch_setup_cycles,
      "serialized write setup cycles");
  const std::uint64_t read_cycles = checked_add(read_transfer_cycles,
                                                read_setup_cycles,
                                                "serialized read cycles");
  const std::uint64_t write_cycles = checked_add(write_transfer_cycles,
                                                 write_setup_cycles,
                                                 "serialized write cycles");
  const std::uint64_t read_compute_cycles = checked_add(read_cycles, compute_cycles,
                                                        "serialized read+compute cycles");
  const std::uint64_t total_cycles = checked_add(read_compute_cycles, write_cycles,
                                                 "serialized total cycles");
  return {read_cycles, compute_cycles, write_cycles, total_cycles};
}

}  // namespace axissim
