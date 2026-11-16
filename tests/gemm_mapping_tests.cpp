#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "gemm_mapping.hpp"

namespace {

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

// Independent scalar oracle: signed 16x16 products fit int32. Conversion to
// uint32 followed by unsigned addition defines modulo-2^32 accumulation, and
// bit_cast reinterprets the resulting output bit pattern without signed UB.
std::vector<std::int32_t> scalar_gemm_oracle(
    std::uint64_t m, std::uint64_t n, std::uint64_t k,
    const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b) {
  require(a.size() == m * k && b.size() == k * n,
          "oracle fixture input sizes must match the requested matrix dimensions");
  std::vector<std::int32_t> c(static_cast<std::size_t>(m * n));
  for (std::uint64_t i = 0; i < m; ++i) {
    for (std::uint64_t j = 0; j < n; ++j) {
      std::uint32_t accumulator_bits = 0;
      for (std::uint64_t inner = 0; inner < k; ++inner) {
        const std::int32_t left = a[static_cast<std::size_t>(i * k + inner)];
        const std::int32_t right = b[static_cast<std::size_t>(inner * n + j)];
        const std::int32_t product = left * right;
        accumulator_bits += static_cast<std::uint32_t>(product);
      }
      c[static_cast<std::size_t>(i * n + j)] =
          std::bit_cast<std::int32_t>(accumulator_bits);
    }
  }
  return c;
}

axissim::GemmWorkload workload(std::uint64_t m, std::uint64_t n,
                               std::uint64_t k) {
  return {m, n, k};
}

void test_one_by_one_work_and_mappings() {
  const auto work = axissim::count_gemm_work(workload(1, 1, 1));
  require_eq(work.useful_macs, 1, "1x1x1 GEMM has one useful MAC");
  require_eq(work.conventional_operation_equivalents, 2,
             "one MAC has two conventional integer operation equivalents");
  require_eq(work.tensor_a_bytes, 2, "one signed i16 A element is two bytes");
  require_eq(work.tensor_b_bytes, 2, "one signed i16 B element is two bytes");
  require_eq(work.tensor_c_bytes, 4, "one signed i32 C element is four bytes");
  require_eq(work.unique_tensor_payload_bytes, 8,
             "1x1x1 unique payload is A+B+C");

  const auto rvv = axissim::estimate_rvv_row_gemm(workload(1, 1, 1), {});
  require_eq(rvv.work.useful_macs, 1,
             "RVV estimate carries the canonical useful-MAC count");
  require_eq(rvv.vlmax, 8, "128-bit VLEN and SEW16 expose VLMAX=8");
  require_eq(rvv.vector_mac_instructions, 1, "1x1x1 needs one RVV MAC instruction");
  require_eq(rvv.compute_cycles, 1, "one active RVV lane slot takes one modeled cycle");
  require_eq(rvv.logical_traffic.total_bytes, 8, "RVV 1x1x1 logical bytes");

  axissim::TiledMatrixGemmMapping matrix_mapping;
  matrix_mapping.tile_m = 1;
  matrix_mapping.tile_n = 1;
  matrix_mapping.tile_k = 1;
  matrix_mapping.local_capacity_bytes = 8;
  const auto matrix = axissim::estimate_tiled_matrix_gemm(
      workload(1, 1, 1), matrix_mapping);
  require_eq(matrix.work.useful_macs, 1,
             "matrix estimate carries the canonical useful-MAC count");
  require_eq(matrix.matrix_tile_operations, 1, "1x1x1 uses one matrix tile operation");
  require_eq(matrix.compute_cycles, 1, "1x1x1 matrix mapping takes one modeled cycle");
  require_eq(matrix.working_set_bytes, 8, "1x1x1 matrix working set is eight bytes");
  require_eq(matrix.logical_traffic.total_bytes, 8, "matrix 1x1x1 logical bytes");
}

void test_rectangular_integer_oracle_and_overflow() {
  const std::vector<std::int16_t> a = {1, 2, 3, 4, -5, 6};
  const std::vector<std::int16_t> b = {7, 8, -9, 10, 11, -12};
  const auto c = scalar_gemm_oracle(2, 2, 3, a, b);
  require(c == std::vector<std::int32_t>({22, -8, 139, -90}),
          "independent 2x2x3 integer oracle must match the hand calculation");
  const auto work = axissim::count_gemm_work(workload(2, 2, 3));
  require_eq(work.useful_macs, 12, "2x2x3 performs twelve useful MACs");
  require_eq(work.conventional_operation_equivalents, 24,
             "2x2x3 has twenty-four conventional operation equivalents");
  require_eq(work.unique_tensor_payload_bytes, 40,
             "2x2x3 unique payload is 12+12+16 bytes");

  const std::vector<std::int16_t> overflow_a = {-32768, -32768};
  const std::vector<std::int16_t> overflow_b = {-32768, -32768};
  const auto overflow_c = scalar_gemm_oracle(1, 1, 2, overflow_a, overflow_b);
  require(overflow_c.at(0) == std::numeric_limits<std::int32_t>::min(),
          "overflow fixture wraps 2147483648 to signed int32 minimum");

  const std::vector<std::int16_t> ones_a(3 * 5, 1);
  const std::vector<std::int16_t> ones_b(5 * 2, 1);
  const auto ones_c = scalar_gemm_oracle(3, 2, 5, ones_a, ones_b);
  require(ones_c == std::vector<std::int32_t>(3 * 2, 5),
          "rectangular scalar oracle computes each output across the full K axis");

  // Validate the mathematical contract for the same short-K, non-divisible
  // M/N shape used by the mapping fixture. The timing model remains shape-only.
  const std::vector<std::int16_t> partial_a(3 * 2, 1);
  const std::vector<std::int16_t> partial_b(2 * 5, 1);
  const auto partial_c = scalar_gemm_oracle(3, 5, 2, partial_a, partial_b);
  require(partial_c == std::vector<std::int32_t>(3 * 5, 2),
          "partial mapping fixture shape computes fifteen outputs of two");
}

void test_hand_derived_mapping_fixtures() {
  const auto tiny_workload = workload(2, 2, 3);
  const auto tiny_rvv = axissim::estimate_rvv_row_gemm(tiny_workload, {});
  require_eq(tiny_rvv.work.useful_macs, 12,
             "tiny RVV estimate reports the common twelve-MAC workload");
  require_eq(tiny_rvv.compute_cycles, 6, "tiny RVV fixture cycles");
  require_eq(tiny_rvv.vector_mac_instructions, 6, "tiny RVV instruction count");
  require_eq(tiny_rvv.logical_traffic.read_a_bytes, 12, "tiny RVV A bytes");
  require_eq(tiny_rvv.logical_traffic.read_b_bytes, 24, "tiny RVV B bytes");
  require_eq(tiny_rvv.logical_traffic.write_c_bytes, 16, "tiny RVV C bytes");
  require_eq(tiny_rvv.logical_traffic.read_a_batches, 6, "tiny RVV A batch count");
  require_eq(tiny_rvv.logical_traffic.read_b_batches, 6, "tiny RVV B batch count");
  require_eq(tiny_rvv.logical_traffic.write_c_batches, 2, "tiny RVV C batch count");

  const auto tiny_matrix = axissim::estimate_tiled_matrix_gemm(
      tiny_workload, axissim::TiledMatrixGemmMapping{});
  require_eq(tiny_matrix.work.useful_macs, 12,
             "tiny matrix estimate reports the common twelve-MAC workload");
  require_eq(tiny_matrix.compute_cycles, 3, "tiny matrix fixture cycles");
  require_eq(tiny_matrix.matrix_tile_operations, 2, "tiny matrix tile operation count");
  require_eq(tiny_matrix.logical_traffic.read_a_bytes, 12, "tiny matrix A bytes");
  require_eq(tiny_matrix.logical_traffic.read_b_bytes, 12, "tiny matrix B bytes");
  require_eq(tiny_matrix.logical_traffic.write_c_bytes, 16, "tiny matrix C bytes");
  require_eq(tiny_matrix.logical_traffic.read_a_batches, 2,
             "tiny matrix performs one A read batch per tile triplet");
  require_eq(tiny_matrix.logical_traffic.read_b_batches, 2,
             "tiny matrix performs one B read batch per tile triplet");
  require_eq(tiny_matrix.logical_traffic.write_c_batches, 1,
             "tiny matrix writes once after its K tiles for the output tile");
  require_eq(tiny_matrix.working_set_bytes, 32, "tiny matrix working set");

  const auto aligned_workload = workload(4, 4, 4);
  const auto aligned_rvv = axissim::estimate_rvv_row_gemm(aligned_workload, {});
  require_eq(aligned_rvv.compute_cycles, 16, "aligned RVV fixture cycles");
  require_eq(aligned_rvv.vector_mac_instructions, 16,
             "aligned RVV fixture instruction count");
  require_eq(aligned_rvv.logical_traffic.read_a_bytes, 32,
             "aligned RVV scalar-A bytes");
  require_eq(aligned_rvv.logical_traffic.read_b_bytes, 128,
             "aligned RVV vector-B bytes");
  axissim::TiledMatrixGemmMapping aligned_mapping;
  aligned_mapping.local_capacity_bytes = 32;
  const auto aligned_matrix = axissim::estimate_tiled_matrix_gemm(
      aligned_workload, aligned_mapping);
  require_eq(aligned_matrix.compute_cycles, 16, "aligned matrix fixture cycles");
  require_eq(aligned_matrix.matrix_tile_operations, 8,
             "aligned matrix tile operation count");
  require_eq(aligned_matrix.logical_traffic.read_a_bytes, 64,
             "aligned matrix A is re-read for each N tile");
  require_eq(aligned_matrix.logical_traffic.read_b_bytes, 64,
             "aligned matrix B is re-read for each M tile");
  require_eq(aligned_matrix.logical_traffic.write_c_bytes, 64,
             "aligned matrix writes each output once");

  const auto partial_workload = workload(3, 5, 2);
  const auto partial_rvv = axissim::estimate_rvv_row_gemm(partial_workload, {});
  require_eq(partial_rvv.work.useful_macs, 30,
             "partial RVV estimate reports the common thirty-MAC workload");
  require_eq(partial_rvv.compute_cycles, 12, "partial RVV fixture cycles");
  require_eq(partial_rvv.vector_mac_instructions, 6,
             "partial RVV tail instruction count");
  require_eq(partial_rvv.logical_traffic.read_a_bytes, 12,
             "partial RVV A bytes");
  require_eq(partial_rvv.logical_traffic.read_b_bytes, 60,
             "partial RVV B bytes");
  axissim::TiledMatrixGemmMapping partial_mapping;
  partial_mapping.tile_m = 2;
  partial_mapping.tile_n = 3;
  partial_mapping.tile_k = 2;
  partial_mapping.local_capacity_bytes = 44;
  const auto partial_matrix = axissim::estimate_tiled_matrix_gemm(
      partial_workload, partial_mapping);
  require_eq(partial_matrix.work.useful_macs, 30,
             "partial matrix estimate reports the common thirty-MAC workload");
  require_eq(partial_matrix.compute_cycles, 10, "partial matrix fixture cycles");
  require_eq(partial_matrix.matrix_tile_operations, 4,
             "partial matrix tile operation count");
  require_eq(partial_matrix.logical_traffic.read_a_bytes, 24,
             "partial matrix A bytes");
  require_eq(partial_matrix.logical_traffic.read_b_bytes, 40,
             "partial matrix B bytes");
  require_eq(partial_matrix.logical_traffic.write_c_bytes, 60,
             "partial matrix C bytes");
  require_eq(partial_matrix.logical_traffic.read_a_batches, 4,
             "partial matrix A batch count equals tile triplet count");
  require_eq(partial_matrix.logical_traffic.read_b_batches, 4,
             "partial matrix B batch count equals tile triplet count");
  require_eq(partial_matrix.logical_traffic.write_c_batches, 4,
             "partial matrix writes once for each M/N output tile");
  require_eq(partial_matrix.working_set_bytes, 44,
             "partial matrix uses the largest edge-compatible tile working set");
  require_eq(partial_matrix.logical_traffic.total_bytes, 124,
             "partial matrix logical bytes reconcile as A+B+C");
}

void test_k_tail_and_reuse_transition() {
  const auto k_tail_workload = workload(3, 5, 3);
  const auto rvv = axissim::estimate_rvv_row_gemm(k_tail_workload, {});
  require_eq(rvv.compute_cycles, 18, "RVV K length three costs three steps per row");
  require_eq(rvv.vector_mac_instructions, 9,
             "RVV instruction count includes all K steps");
  require_eq(rvv.logical_traffic.read_a_bytes, 18, "RVV K-tail A bytes");
  require_eq(rvv.logical_traffic.read_b_bytes, 90, "RVV K-tail B bytes");

  axissim::TiledMatrixGemmMapping tiled;
  tiled.tile_m = 2;
  tiled.tile_n = 3;
  tiled.tile_k = 2;
  tiled.local_capacity_bytes = 44;
  const auto matrix = axissim::estimate_tiled_matrix_gemm(k_tail_workload, tiled);
  // Independent schedule sum: output tile extents are (2,3), (2,2),
  // (1,3), (1,2). Their per-K-step service cycles at P=4 are 2,1,1,1;
  // summing each over K=3 gives 6+3+3+3=15 cycles.
  require_eq(matrix.compute_cycles, 15, "matrix mapping accounts for K and M/N edge tiles");
  require_eq(matrix.matrix_tile_operations, 8,
             "two M, two N, and two K tiles form eight operations");
  require_eq(matrix.logical_traffic.read_a_bytes, 36, "matrix K-tail A bytes");
  require_eq(matrix.logical_traffic.read_b_bytes, 60, "matrix K-tail B bytes");
  require_eq(matrix.logical_traffic.write_c_bytes, 60, "matrix K-tail C bytes");
  require_eq(matrix.working_set_bytes, 44, "K-tail matrix working set");
  require((45 + 4 - 1) / 4 == 12,
          "test fixture lower-bound arithmetic is independently established");
  require(matrix.compute_cycles >= 12,
          "tiled schedule cannot beat the ideal aggregate MAC lower bound");

  axissim::TiledMatrixGemmMapping wider_tiles;
  wider_tiles.tile_m = 4;
  wider_tiles.tile_n = 5;
  wider_tiles.tile_k = 2;
  wider_tiles.local_capacity_bytes = 116;
  const auto reused = axissim::estimate_tiled_matrix_gemm(workload(3, 5, 2),
                                                           wider_tiles);
  const auto smaller = axissim::estimate_tiled_matrix_gemm(
      workload(3, 5, 2), axissim::TiledMatrixGemmMapping{2, 3, 2, 44, 4, 1, 0, 0});
  require_eq(reused.work.useful_macs, smaller.work.useful_macs,
             "tile changes must not alter mathematical work");
  require(reused.logical_traffic.total_bytes < smaller.logical_traffic.total_bytes,
          "reducing outer tiling reuse in this fixture reduces mapping-derived bytes");
  require(reused.compute_cycles <= smaller.compute_cycles,
          "the larger output tile may reduce fragmentation, not increase this schedule's cycles");
}

void test_capacity_and_parameter_validation() {
  const auto partial = workload(3, 5, 2);
  axissim::TiledMatrixGemmMapping exactly_fits{2, 3, 2, 44, 4, 1, 0, 0};
  require_eq(axissim::estimate_tiled_matrix_gemm(partial, exactly_fits).working_set_bytes,
             44, "a tile exactly at capacity is accepted");
  exactly_fits.local_capacity_bytes = 43;
  require_throws<std::length_error>(
      [&] { (void)axissim::estimate_tiled_matrix_gemm(partial, exactly_fits); },
      "a tile one byte over local capacity must be rejected");

  require_throws<std::invalid_argument>(
      [] { (void)axissim::count_gemm_work(workload(0, 1, 1)); },
      "zero M dimension must be rejected");
  auto unsupported = workload(1, 1, 1);
  unsupported.layout = static_cast<axissim::GemmLayout>(99);
  require_throws<std::invalid_argument>(
      [&] { (void)axissim::count_gemm_work(unsupported); },
      "unsupported layout must be rejected");

  axissim::RvvGemmMapping bad_rvv;
  bad_rvv.vlen_bits = 192;
  require_throws<std::invalid_argument>(
      [&] { (void)axissim::estimate_rvv_row_gemm(workload(1, 1, 1), bad_rvv); },
      "non-power-of-two RVV VLEN must be rejected");
  bad_rvv = {};
  bad_rvv.accumulator_recurrence_cycles = 0;
  require_throws<std::invalid_argument>(
      [&] { (void)axissim::estimate_rvv_row_gemm(workload(1, 1, 1), bad_rvv); },
      "zero RVV recurrence must be rejected");

  axissim::TiledMatrixGemmMapping bad_matrix;
  bad_matrix.tile_k = 0;
  require_throws<std::invalid_argument>(
      [&] { (void)axissim::estimate_tiled_matrix_gemm(workload(1, 1, 1), bad_matrix); },
      "zero matrix tile size must be rejected");
}

void test_checked_overflow_paths() {
  require_throws<std::overflow_error>(
      [] {
        (void)axissim::count_gemm_work(
            workload(std::numeric_limits<std::uint64_t>::max(), 2, 1));
      },
      "dimension product overflow must be detected");
  require_throws<std::overflow_error>(
      [] { (void)axissim::count_gemm_work(workload(1ULL << 31, 1ULL << 31, 1)); },
      "C tensor byte-count overflow must be detected independently of useful MAC count");
  require_throws<std::overflow_error>(
      [] { (void)axissim::count_gemm_work(workload(1, 1, 1ULL << 62)); },
      "unique A+B logical payload addition overflow must be detected");

  axissim::TiledMatrixGemmMapping traffic_overflow;
  traffic_overflow.tile_m = 1;
  traffic_overflow.tile_n = 1;
  traffic_overflow.tile_k = 1;
  traffic_overflow.local_capacity_bytes = 8;
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_tiled_matrix_gemm(
            workload(1, 2, 1ULL << 61), traffic_overflow);
      },
      "matrix A+B mapping traffic addition overflow must be detected");

  axissim::RvvGemmMapping too_many_slots;
  too_many_slots.lanes = std::numeric_limits<std::uint64_t>::max();
  too_many_slots.macs_per_lane_cycle = 2;
  require_throws<std::overflow_error>(
      [&] { (void)axissim::estimate_rvv_row_gemm(workload(1, 1, 1), too_many_slots); },
      "RVV slot-count multiplication overflow must be detected");

  axissim::RvvGemmMapping cycle_overflow;
  cycle_overflow.kernel_setup_cycles = std::numeric_limits<std::uint64_t>::max();
  cycle_overflow.chunk_setup_cycles = 1;
  require_throws<std::overflow_error>(
      [&] { (void)axissim::estimate_rvv_row_gemm(workload(1, 1, 1), cycle_overflow); },
      "mapping setup-cycle addition overflow must be detected");

  axissim::TiledMatrixGemmMapping matrix_cycle_overflow;
  matrix_cycle_overflow.tile_m = 1;
  matrix_cycle_overflow.tile_n = 1;
  matrix_cycle_overflow.tile_k = 1;
  matrix_cycle_overflow.local_capacity_bytes = 8;
  matrix_cycle_overflow.kernel_setup_cycles =
      std::numeric_limits<std::uint64_t>::max();
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_tiled_matrix_gemm(workload(1, 1, 1),
                                                  matrix_cycle_overflow);
      },
      "matrix kernel setup plus compute overflow must be detected");

  matrix_cycle_overflow.kernel_setup_cycles = 0;
  matrix_cycle_overflow.tile_setup_cycles =
      std::numeric_limits<std::uint64_t>::max();
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_tiled_matrix_gemm(workload(1, 1, 1),
                                                  matrix_cycle_overflow);
      },
      "matrix tile setup plus tile compute overflow must be detected");

  matrix_cycle_overflow.tile_setup_cycles = 0;
  matrix_cycle_overflow.accumulator_recurrence_cycles =
      std::numeric_limits<std::uint64_t>::max();
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_tiled_matrix_gemm(workload(1, 1, 2),
                                                  matrix_cycle_overflow);
      },
      "matrix repeated tile recurrence overflow must be detected");

  axissim::SerializedBackingStoreParameters bandwidth_overflow{
      1, 0};
  const axissim::GemmLogicalTraffic huge_traffic{
      std::numeric_limits<std::uint64_t>::max(), 0, 0,
      std::numeric_limits<std::uint64_t>::max(), 0, 0, 0};
  require_throws<std::overflow_error>(
      [&] { (void)axissim::estimate_serialized_backing_store(
          huge_traffic, 0, bandwidth_overflow); },
      "serialized byte-times-clock multiplication overflow must be detected");

  const axissim::GemmLogicalTraffic huge_write_traffic{
      0, 0, std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), 0, 0,
      std::numeric_limits<std::uint64_t>::max()};
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_serialized_backing_store(
            huge_write_traffic, 0, bandwidth_overflow);
      },
      "serialized write byte-times-clock multiplication overflow must be detected");

  const axissim::GemmLogicalTraffic huge_write_batch_traffic{
      0, 0, 0, 0, 0, 0, std::numeric_limits<std::uint64_t>::max()};
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_serialized_backing_store(
            huge_write_batch_traffic, 0, {1'000'000'000, 2});
      },
      "serialized write setup multiplication overflow must be detected");

  const axissim::GemmLogicalTraffic huge_read_batch_traffic{
      0, 0, 0, 0, std::numeric_limits<std::uint64_t>::max(), 0, 0};
  require_throws<std::overflow_error>(
      [&] {
        (void)axissim::estimate_serialized_backing_store(
            huge_read_batch_traffic, 0, {1'000'000'000, 2});
      },
      "serialized read setup multiplication overflow must be detected");
}

void test_capacity_and_throughput_sensitivities() {
  const auto input = workload(3, 5, 2);
  const auto base_rvv = axissim::estimate_rvv_row_gemm(input, {});
  axissim::RvvGemmMapping wider_rvv;
  wider_rvv.lanes = 8;
  require(axissim::estimate_rvv_row_gemm(input, wider_rvv).compute_cycles <=
              base_rvv.compute_cycles,
          "more RVV lanes cannot increase cycles in the declared analytical model");

  const auto base_matrix = axissim::estimate_tiled_matrix_gemm(
      input, axissim::TiledMatrixGemmMapping{2, 3, 2, 44, 4, 1, 0, 0});
  auto more_slots = axissim::TiledMatrixGemmMapping{2, 3, 2, 44, 8, 1, 0, 0};
  require(axissim::estimate_tiled_matrix_gemm(input, more_slots).compute_cycles <=
              base_matrix.compute_cycles,
          "more matrix MAC slots cannot increase cycles in the declared model");

  axissim::SerializedBackingStoreParameters slow{1'000'000'000, 0};
  axissim::SerializedBackingStoreParameters fast{2'000'000'000, 0};
  const auto slow_result = axissim::estimate_serialized_backing_store(
      base_rvv.logical_traffic, base_rvv.compute_cycles, slow);
  const auto fast_result = axissim::estimate_serialized_backing_store(
      base_rvv.logical_traffic, base_rvv.compute_cycles, fast);
  require(fast_result.total_cycles <= slow_result.total_cycles,
          "higher declared backing-store bandwidth cannot increase serialized cycles");
  require_eq(fast_result.read_cycles, 36,
             "72 read bytes at 2 GB/s take 36 cycles");
  require_eq(fast_result.compute_cycles, 12,
             "2 GB/s sensitivity preserves the compute-cycle estimate");
  require_eq(fast_result.write_cycles, 30,
             "60 output bytes at 2 GB/s take 30 cycles");
  require_eq(fast_result.total_cycles, 78,
             "serialized 2 GB/s total is read plus compute plus write");
  require_eq(slow_result.compute_cycles, base_rvv.compute_cycles,
             "serialized sensitivity reports, but does not replace, compute cycles");
  require(fast_result.compute_cycles == base_rvv.compute_cycles,
          "bandwidth sensitivity leaves the primary compute estimate unchanged");
  const auto exact_one_gbps = axissim::estimate_serialized_backing_store(
      base_rvv.logical_traffic, base_rvv.compute_cycles,
      {1'000'000'000, 0});
  require_eq(exact_one_gbps.read_cycles, 72,
             "72 read bytes at 1 GB/s take 72 cycles at the fixed 1 GHz clock");
  require_eq(exact_one_gbps.compute_cycles, 12,
             "serialized output preserves hand-counted compute cycles");
  require_eq(exact_one_gbps.write_cycles, 60,
             "60 output bytes at 1 GB/s take 60 cycles at the fixed 1 GHz clock");
  require_eq(exact_one_gbps.total_cycles, 144,
             "serialized 1 GB/s total is read plus compute plus write");

  const auto non_divisible_bandwidth = axissim::estimate_serialized_backing_store(
      base_rvv.logical_traffic, base_rvv.compute_cycles,
      {1'400'000'000, 0});
  require_eq(non_divisible_bandwidth.read_cycles, 52,
             "non-integral 72/1.4 ratio rounds read transfer cycles up");
  require_eq(non_divisible_bandwidth.write_cycles, 43,
             "non-integral 60/1.4 ratio rounds write transfer cycles up");
  require_eq(non_divisible_bandwidth.total_cycles, 107,
             "non-divisible bandwidth total uses ceiling transfer service");

  const auto exact_with_setup = axissim::estimate_serialized_backing_store(
      base_rvv.logical_traffic, base_rvv.compute_cycles,
      {1'000'000'000, 1});
  require_eq(exact_with_setup.read_cycles, 84,
             "one setup cycle is charged to each of twelve read batches");
  require_eq(exact_with_setup.write_cycles, 63,
             "one setup cycle is charged to each of three output batches");
  require_eq(exact_with_setup.total_cycles, 159,
             "serialized setup total reconciles with read and write batches");
  const auto with_setup = axissim::estimate_serialized_backing_store(
      base_rvv.logical_traffic, base_rvv.compute_cycles,
      {1'000'000'000, 1});
  require(with_setup.total_cycles >= slow_result.total_cycles,
          "additional batch setup cannot reduce serialized cycles");
  require(with_setup.read_cycles > slow_result.read_cycles &&
              with_setup.write_cycles > slow_result.write_cycles,
          "declared batch setup is charged at the read and output boundaries");

  require_throws<std::invalid_argument>(
      [&] {
        (void)axissim::estimate_serialized_backing_store(
            base_rvv.logical_traffic, base_rvv.compute_cycles, {0, 0});
      },
      "zero backing-store bandwidth must be rejected");
  auto inconsistent = base_rvv.logical_traffic;
  ++inconsistent.total_bytes;
  require_throws<std::invalid_argument>(
      [&] {
        (void)axissim::estimate_serialized_backing_store(
            inconsistent, base_rvv.compute_cycles, slow);
      },
      "inconsistent logical byte totals must be rejected");
}

void test_repeatability() {
  const auto input = workload(3, 5, 3);
  const axissim::RvvGemmMapping rvv_mapping{128, 4, 1, 1, 3, 2};
  const axissim::TiledMatrixGemmMapping matrix_mapping{2, 3, 2, 44, 4, 1, 5, 1};
  const auto rvv_first = axissim::estimate_rvv_row_gemm(input, rvv_mapping);
  const auto rvv_second = axissim::estimate_rvv_row_gemm(input, rvv_mapping);
  require(rvv_first.compute_cycles == rvv_second.compute_cycles &&
              rvv_first.vector_mac_instructions == rvv_second.vector_mac_instructions &&
              rvv_first.logical_traffic.total_bytes == rvv_second.logical_traffic.total_bytes,
          "identical RVV inputs must produce identical estimates");
  const auto matrix_first = axissim::estimate_tiled_matrix_gemm(input, matrix_mapping);
  const auto matrix_second = axissim::estimate_tiled_matrix_gemm(input, matrix_mapping);
  require(matrix_first.compute_cycles == matrix_second.compute_cycles &&
              matrix_first.matrix_tile_operations == matrix_second.matrix_tile_operations &&
              matrix_first.logical_traffic.total_bytes == matrix_second.logical_traffic.total_bytes,
          "identical matrix inputs must produce identical estimates");
}

}  // namespace

int main() {
  try {
    test_one_by_one_work_and_mappings();
    test_rectangular_integer_oracle_and_overflow();
    test_hand_derived_mapping_fixtures();
    test_k_tail_and_reuse_transition();
    test_capacity_and_parameter_validation();
    test_checked_overflow_paths();
    test_capacity_and_throughput_sensitivities();
    test_repeatability();
    std::cout << "GEMM mapping tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
