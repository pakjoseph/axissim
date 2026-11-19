#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "matrix_architecture.hpp"

namespace {

using axissim::MatrixArchitectureState;
using axissim::MatrixBankCapacities;
using axissim::MatrixCommand;
using axissim::MatrixCommandError;
using axissim::MatrixOpcode;
using axissim::MatrixWorkShape;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_error(MatrixArchitectureState& state, const MatrixCommand& command,
                   MatrixCommandError expected, const std::string& message) {
  const auto result = state.apply(command);
  require(result.error == expected,
          message + " (expected " +
              std::string(axissim::matrix_command_error_name(expected)) +
              ", got " + std::string(axissim::matrix_command_error_name(result.error)) +
              ")");
}

[[nodiscard]] std::uint64_t ceil_div(std::uint64_t value,
                                     std::uint64_t divisor) {
  return value / divisor + static_cast<std::uint64_t>(value % divisor != 0);
}

// The command generator is test-owned. It independently describes the
// approved M-major/N-major output traversal and ascending K-panel lifecycle.
std::vector<MatrixCommand> reference_commands(const MatrixWorkShape& shape) {
  std::vector<MatrixCommand> commands;
  std::uint64_t sequence = 0;
  for (std::uint64_t im = 0; im < ceil_div(shape.m, shape.tile_m); ++im) {
    const std::uint64_t valid_m =
        std::min(shape.tile_m, shape.m - im * shape.tile_m);
    for (std::uint64_t in = 0; in < ceil_div(shape.n, shape.tile_n); ++in) {
      const std::uint64_t valid_n =
          std::min(shape.tile_n, shape.n - in * shape.tile_n);
      commands.push_back({sequence++, MatrixOpcode::clear_accumulator,
                          im, in, 0, valid_m, valid_n, 0});
      for (std::uint64_t ik = 0; ik < ceil_div(shape.k, shape.tile_k); ++ik) {
        const std::uint64_t valid_k =
            std::min(shape.tile_k, shape.k - ik * shape.tile_k);
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

// Independent scalar GEMM oracle. The product is formed in int64, which safely
// contains every signed-i16 product. Unsigned addition and bit_cast define the
// requested modulo-2^32 result without signed overflow.
std::vector<std::int32_t> scalar_oracle(
    std::uint64_t m, std::uint64_t n, std::uint64_t k,
    const std::vector<std::int16_t>& a,
    const std::vector<std::int16_t>& b) {
  std::vector<std::int32_t> c(static_cast<std::size_t>(m * n), 0);
  for (std::uint64_t row = 0; row < m; ++row) {
    for (std::uint64_t column = 0; column < n; ++column) {
      std::uint32_t sum = 0;
      for (std::uint64_t inner = 0; inner < k; ++inner) {
        const std::int64_t product =
            static_cast<std::int64_t>(a[static_cast<std::size_t>(row * k + inner)]) *
            static_cast<std::int64_t>(b[static_cast<std::size_t>(inner * n + column)]);
        sum += static_cast<std::uint32_t>(static_cast<std::int32_t>(product));
      }
      c[static_cast<std::size_t>(row * n + column)] =
          std::bit_cast<std::int32_t>(sum);
    }
  }
  return c;
}

[[nodiscard]] MatrixBankCapacities capacities_for(const MatrixWorkShape& shape) {
  return {shape.tile_m * shape.tile_k * 2,
          shape.tile_k * shape.tile_n * 2,
          shape.tile_m * shape.tile_n * 4};
}

void run_reference_commands(MatrixArchitectureState& state,
                            const std::vector<MatrixCommand>& commands) {
  for (const auto& command : commands) {
    const auto result = state.apply(command);
    require(result.accepted(), "reference command must be accepted; error=" +
                                   std::string(axissim::matrix_command_error_name(
                                       result.error)));
  }
}

void run_and_check(MatrixWorkShape shape, std::vector<std::int16_t> a,
                   std::vector<std::int16_t> b) {
  require(a.size() == shape.m * shape.k, "fixture A size must match shape");
  require(b.size() == shape.k * shape.n, "fixture B size must match shape");
  const auto expected = scalar_oracle(shape.m, shape.n, shape.k, a, b);
  std::vector<std::int32_t> c(static_cast<std::size_t>(shape.m * shape.n), -777);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);
  const auto commands = reference_commands(shape);
  run_reference_commands(state, commands);
  require(state.complete(), "all ordered output tiles must complete");
  require(c == expected, "matrix command model must match independent scalar GEMM");
  require(state.next_sequence_id() == commands.size(),
          "one sequence ID must be consumed by each accepted command");

  std::vector<std::int32_t> repeated_c(static_cast<std::size_t>(shape.m * shape.n), -777);
  MatrixArchitectureState repeated_state(shape, capacities_for(shape), a, b, repeated_c);
  run_reference_commands(repeated_state, commands);
  require(repeated_state.complete() && repeated_c == c &&
              repeated_state.next_sequence_id() == state.next_sequence_id(),
          "a fresh state with identical inputs and commands must reproduce tensor and sequence");
}

void test_scalar_oracle_shapes_and_k_panels() {
  run_and_check({1, 1, 1, 1, 1, 1}, {7}, {-9});

  std::vector<std::int16_t> rectangular_a = {1, -2, 3, 4, 5, -6, 7, 8};
  std::vector<std::int16_t> rectangular_b = {
      2, 3, -4, 5, 6, 7, 8, -9, 10, 11, -12, 13};
  run_and_check({2, 3, 4, 1, 2, 3}, rectangular_a, rectangular_b);

  std::vector<std::int16_t> partial_m_a(3 * 5);
  std::vector<std::int16_t> partial_m_b(5 * 2);
  for (std::size_t i = 0; i < partial_m_a.size(); ++i) {
    partial_m_a[i] = static_cast<std::int16_t>(static_cast<int>(i) - 4);
  }
  for (std::size_t i = 0; i < partial_m_b.size(); ++i) {
    partial_m_b[i] = static_cast<std::int16_t>(static_cast<int>(i % 7) - 3);
  }
  run_and_check({3, 2, 5, 2, 1, 2}, partial_m_a, partial_m_b);
}

void test_5x7x3_clipped_extents_and_row_major_offsets() {
  const MatrixWorkShape shape{5, 7, 3, 4, 4, 2};
  std::vector<std::int16_t> a(15);
  std::vector<std::int16_t> b(21);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] = static_cast<std::int16_t>(i + 1);
  }
  for (std::size_t i = 0; i < b.size(); ++i) {
    b[i] = static_cast<std::int16_t>(101 + i);
  }

  const auto commands = reference_commands(shape);
  bool saw_final_a = false;
  bool saw_final_b = false;
  bool saw_final_mac = false;
  for (const auto& command : commands) {
    if (command.tile_m_index != 1 || command.tile_n_index != 1 ||
        command.k_panel_index != 1) {
      continue;
    }
    if (command.opcode == MatrixOpcode::load_a) {
      saw_final_a = command.valid_m == 1 && command.valid_n == 0 &&
                    command.valid_k == 1;
    } else if (command.opcode == MatrixOpcode::load_b) {
      saw_final_b = command.valid_m == 0 && command.valid_n == 3 &&
                    command.valid_k == 1;
    } else if (command.opcode == MatrixOpcode::matrix_mac_accumulate) {
      saw_final_mac = command.valid_m == 1 && command.valid_n == 3 &&
                      command.valid_k == 1;
    }
  }
  require(saw_final_a && saw_final_b && saw_final_mac,
          "last tile and panel must clip to valid extents (1,3,1)");

  std::vector<std::int32_t> c(35, -999);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);
  run_reference_commands(state, commands);
  require(state.complete(), "5x7x3 traversal must finish all six output tiles");
  require(c == scalar_oracle(5, 7, 3, a, b),
          "5x7x3 tiled result must match the independent scalar oracle");

  // Hand-derived final row: A[12:15]={13,14,15}; B rows 0..2, columns 4..6
  // are {105..107}, {112..114}, {119..121}.
  require(c[32] == 13 * 105 + 14 * 112 + 15 * 119,
          "final output row starts at row-major C element 32");
  require(c[33] == 13 * 106 + 14 * 113 + 15 * 120,
          "final output column uses row-major B column offset 5");
  require(c[34] == 13 * 107 + 14 * 114 + 15 * 121,
          "final output column uses row-major B column offset 6");
}

void test_modulo_overflow_across_k_panels() {
  const MatrixWorkShape shape{1, 1, 2, 1, 1, 1};
  const std::vector<std::int16_t> a{-32768, -32768};
  const std::vector<std::int16_t> b{-32768, -32768};
  std::vector<std::int32_t> c(1, 0);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);
  run_reference_commands(state, reference_commands(shape));
  require(c[0] == std::numeric_limits<std::int32_t>::min(),
          "two 32768x32768 products wrap to signed i32 minimum");
}

void test_deterministic_trace_order_and_sequence_ids() {
  const MatrixWorkShape shape{5, 7, 3, 4, 4, 2};
  const auto first = reference_commands(shape);
  const auto second = reference_commands(shape);
  require(first == second, "repeated command traces must be identical");
  require(first.size() == 4 * (2 + 3 * 2),
          "four output tiles and two K panels use O*(2+3Q) commands");
  for (std::size_t i = 0; i < first.size(); ++i) {
    require(first[i].sequence_id == i,
            "trace sequence IDs must be consecutive and stable");
  }
  require(first.front().opcode == MatrixOpcode::clear_accumulator &&
              first.front().tile_m_index == 0 && first.front().tile_n_index == 0,
          "trace starts at output tile (0,0)");
  const auto last = first.back();
  require(last.opcode == MatrixOpcode::store_accumulator_tile &&
              last.tile_m_index == 1 && last.tile_n_index == 1,
          "M-major/N-major trace ends at output tile (1,1)");
}

void test_canonical_unused_fields_and_retry() {
  const MatrixWorkShape shape{1, 1, 1, 1, 1, 1};
  const std::vector<std::int16_t> a{3};
  const std::vector<std::int16_t> b{4};
  std::vector<std::int32_t> c(1, -50);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);

  MatrixCommand command{0, MatrixOpcode::clear_accumulator, 0, 0, 1, 1, 1, 0};
  require_error(state, command, MatrixCommandError::unused_field_nonzero,
                "clear rejects nonzero unused K panel");
  require(state.next_sequence_id() == 0 && c[0] == -50,
          "rejected clear preserves sequence and C");
  command.k_panel_index = 0;
  command.valid_k = 1;
  require_error(state, command, MatrixCommandError::unused_field_nonzero,
                "clear rejects nonzero unused valid_k");
  command.valid_k = 0;
  require(state.apply(command).accepted(), "canonical clear is accepted at same ID");

  MatrixCommand load_a{1, MatrixOpcode::load_a, 0, 0, 0, 1, 9, 1};
  require_error(state, load_a, MatrixCommandError::unused_field_nonzero,
                "LoadA rejects nonzero unused valid_n");
  load_a.valid_n = 0;
  require(state.apply(load_a).accepted(), "canonical LoadA is accepted at same ID");

  MatrixCommand load_b{2, MatrixOpcode::load_b, 0, 0, 0, 9, 1, 1};
  require_error(state, load_b, MatrixCommandError::unused_field_nonzero,
                "LoadB rejects nonzero unused valid_m");
  load_b.valid_m = 0;
  require(state.apply(load_b).accepted(), "canonical LoadB is accepted at same ID");

  MatrixCommand bad_mac_extent{3, MatrixOpcode::matrix_mac_accumulate,
                               0, 0, 0, 1, 1, 2};
  require_error(state, bad_mac_extent, MatrixCommandError::invalid_extent,
                "MAC with a command extent different from the derived tile is rejected");
  require(state.next_sequence_id() == 3 && c[0] == -50,
          "invalid MAC extent preserves loaded operands, C, and sequence");

  MatrixCommand mac{3, MatrixOpcode::matrix_mac_accumulate, 0, 0, 0, 1, 1, 1};
  require(state.apply(mac).accepted(), "MAC command is accepted");
  MatrixCommand store{4, MatrixOpcode::store_accumulator_tile, 0, 0, 1, 1, 1, 0};
  require_error(state, store, MatrixCommandError::unused_field_nonzero,
                "store rejects nonzero unused K panel");
  store.k_panel_index = 0;
  store.valid_k = 1;
  require_error(state, store, MatrixCommandError::unused_field_nonzero,
                "store rejects nonzero unused valid_k");
  store.valid_k = 0;
  require(c[0] == -50 && state.next_sequence_id() == 4,
          "rejected store preserves output and expected sequence");
  require(state.apply(store).accepted(), "canonical store succeeds at same ID");
  require(c[0] == 12 && state.complete(), "canonical command sequence stores result");
}

void test_rejected_command_failure_atomicity() {
  const MatrixWorkShape shape{1, 1, 2, 1, 1, 1};
  const std::vector<std::int16_t> a{3, 4};
  const std::vector<std::int16_t> b{5, 6};
  std::vector<std::int32_t> c(1, -77);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);

  auto clear = MatrixCommand{0, MatrixOpcode::clear_accumulator, 0, 0, 0, 1, 1, 0};
  require(state.apply(clear).accepted(), "initial clear succeeds");
  auto load_a = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
  require(state.apply(load_a).accepted(), "first A panel loads");

  auto duplicate_a = MatrixCommand{2, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
  require_error(state, duplicate_a, MatrixCommandError::slot_busy,
                "overwriting a live A slot is rejected");
  require(state.next_sequence_id() == 2 && c[0] == -77,
          "rejected A overwrite preserves sequence and output");

  auto load_b = MatrixCommand{2, MatrixOpcode::load_b, 0, 0, 0, 0, 1, 1};
  require(state.apply(load_b).accepted(), "B panel loads using unconsumed sequence ID");
  auto mac = MatrixCommand{3, MatrixOpcode::matrix_mac_accumulate, 0, 0, 0, 1, 1, 1};
  require(state.apply(mac).accepted(), "first K panel accumulates");

  auto wrong_panel = MatrixCommand{4, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
  wrong_panel.k_panel_index = 0;
  require_error(state, wrong_panel, MatrixCommandError::wrong_k_panel,
                "stale K panel identity is rejected");
  require(state.next_sequence_id() == 4 && c[0] == -77,
          "wrong panel rejection leaves accumulated state unpublished");

  auto second_a = MatrixCommand{4, MatrixOpcode::load_a, 0, 0, 1, 1, 0, 1};
  require(state.apply(second_a).accepted(), "second A panel loads");
  auto missing_b_mac = MatrixCommand{5, MatrixOpcode::matrix_mac_accumulate,
                                     0, 0, 1, 1, 1, 1};
  require_error(state, missing_b_mac, MatrixCommandError::missing_operand,
                "MAC without the matching B panel is rejected");
  require(state.next_sequence_id() == 5 && c[0] == -77,
          "rejected MAC preserves sequence, accumulator, and output");

  auto second_b = MatrixCommand{5, MatrixOpcode::load_b, 0, 0, 1, 0, 1, 1};
  require(state.apply(second_b).accepted(), "B panel retries at preserved sequence ID");
  auto second_mac = MatrixCommand{6, MatrixOpcode::matrix_mac_accumulate,
                                  0, 0, 1, 1, 1, 1};
  require(state.apply(second_mac).accepted(), "second K panel accumulates");

  auto wrong_store_tile = MatrixCommand{7, MatrixOpcode::store_accumulator_tile,
                                        0, 0, 0, 1, 1, 0};
  wrong_store_tile.valid_k = 1;
  require_error(state, wrong_store_tile, MatrixCommandError::unused_field_nonzero,
                "rejected store cannot alter output contents");
  require(state.next_sequence_id() == 7 && c[0] == -77,
          "failed store preserves C, accumulated value, and sequence");
  auto store = MatrixCommand{7, MatrixOpcode::store_accumulator_tile,
                             0, 0, 0, 1, 1, 0};
  require(state.apply(store).accepted(), "store retries at the preserved sequence ID");
  require(c[0] == 3 * 5 + 4 * 6,
          "final output proves loaded operands and prior accumulator survived rejection");

  auto duplicate_store = MatrixCommand{8, MatrixOpcode::store_accumulator_tile,
                                       0, 0, 0, 1, 1, 0};
  require_error(state, duplicate_store, MatrixCommandError::duplicate_store,
                "storing the same completed tile twice is rejected");
  require(state.next_sequence_id() == 8 && c[0] == 39,
          "duplicate store rejection leaves C and sequence unchanged");
}

void test_wrong_tile_and_wrong_panel_do_not_consume_sequence() {
  const MatrixWorkShape shape{2, 2, 2, 1, 1, 1};
  const std::vector<std::int16_t> a{1, 2, 3, 4};
  const std::vector<std::int16_t> b{5, 6, 7, 8};
  std::vector<std::int32_t> c(4, -4);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);
  auto clear = MatrixCommand{0, MatrixOpcode::clear_accumulator, 0, 1, 0, 1, 1, 0};
  require_error(state, clear, MatrixCommandError::wrong_tile,
                "first output tile must be M-major/N-major (0,0)");
  require(state.next_sequence_id() == 0, "wrong tile does not consume ID");
  clear.tile_n_index = 0;
  require(state.apply(clear).accepted(), "correct first tile clear retries");

  auto wrong_panel = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 1, 1, 0, 1};
  require_error(state, wrong_panel, MatrixCommandError::wrong_k_panel,
                "valid but future K panel is rejected");
  require(state.next_sequence_id() == 1,
          "wrong panel does not consume expected sequence ID");
  auto first_panel = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
  require(state.apply(first_panel).accepted(), "correct panel retries at same ID");
}

void test_invalid_indices_and_clipped_extents_are_rejected() {
  const MatrixWorkShape shape{2, 3, 2, 1, 2, 1};
  const std::vector<std::int16_t> a{1, 2, 3, 4};
  const std::vector<std::int16_t> b{5, 6, 7, 8, 9, 10};
  std::vector<std::int32_t> c(6, -6);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);

  auto invalid_tile = MatrixCommand{0, MatrixOpcode::clear_accumulator,
                                    2, 0, 0, 1, 2, 0};
  require_error(state, invalid_tile, MatrixCommandError::invalid_extent,
                "out-of-range M tile index is rejected");
  require(state.next_sequence_id() == 0 && c == std::vector<std::int32_t>(6, -6),
          "out-of-range tile rejection preserves state and C");
  auto clear = MatrixCommand{0, MatrixOpcode::clear_accumulator, 0, 0, 0, 1, 2, 0};
  require(state.apply(clear).accepted(), "valid first tile clear follows rejection");

  auto invalid_panel = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 2, 1, 0, 1};
  require_error(state, invalid_panel, MatrixCommandError::invalid_extent,
                "out-of-range K panel index is rejected");
  auto invalid_clip = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 2};
  require_error(state, invalid_clip, MatrixCommandError::invalid_extent,
                "caller cannot widen the derived K-panel extent");
  require(state.next_sequence_id() == 1 && c == std::vector<std::int32_t>(6, -6),
          "invalid panel and extent preserve sequence and output");
  auto correct_load = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
  require(state.apply(correct_load).accepted(), "correct clipped extent retries at same ID");
}

void test_capacity_boundaries_are_atomic() {
  const MatrixWorkShape shape{1, 1, 1, 1, 1, 1};
  const std::vector<std::int16_t> a{2};
  const std::vector<std::int16_t> b{3};

  {
    std::vector<std::int32_t> c(1, -10);
    MatrixArchitectureState state(shape, {2, 2, 4}, a, b, c);
    run_reference_commands(state, reference_commands(shape));
    require(c[0] == 6, "exact A/B/accumulator capacity boundaries are accepted");
  }
  {
    std::vector<std::int32_t> c(1, -10);
    MatrixArchitectureState state(shape, {2, 2, 3}, a, b, c);
    auto clear = MatrixCommand{0, MatrixOpcode::clear_accumulator, 0, 0, 0, 1, 1, 0};
    require_error(state, clear, MatrixCommandError::capacity_exceeded,
                  "one-byte-short accumulator capacity is rejected");
    require(state.next_sequence_id() == 0 && c[0] == -10,
            "accumulator capacity rejection is atomic");
  }
  {
    std::vector<std::int32_t> c(1, -10);
    MatrixArchitectureState state(shape, {1, 2, 4}, a, b, c);
    auto clear = MatrixCommand{0, MatrixOpcode::clear_accumulator, 0, 0, 0, 1, 1, 0};
    require(state.apply(clear).accepted(), "clear before A capacity rejection");
    auto load_a = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
    require_error(state, load_a, MatrixCommandError::capacity_exceeded,
                  "one-byte-short A bank capacity is rejected");
    require(state.next_sequence_id() == 1 && c[0] == -10,
            "A capacity rejection preserves active accumulator and sequence");
  }
  {
    std::vector<std::int32_t> c(1, -10);
    MatrixArchitectureState state(shape, {2, 1, 4}, a, b, c);
    auto clear = MatrixCommand{0, MatrixOpcode::clear_accumulator, 0, 0, 0, 1, 1, 0};
    auto load_a = MatrixCommand{1, MatrixOpcode::load_a, 0, 0, 0, 1, 0, 1};
    auto load_b = MatrixCommand{2, MatrixOpcode::load_b, 0, 0, 0, 0, 1, 1};
    require(state.apply(clear).accepted() && state.apply(load_a).accepted(),
            "setup for B capacity check succeeds");
    require_error(state, load_b, MatrixCommandError::capacity_exceeded,
                  "one-byte-short B bank capacity is rejected");
    require(state.next_sequence_id() == 2 && c[0] == -10,
            "B capacity rejection preserves A slot and sequence");
  }
}

void test_invalid_context_and_checked_sizes() {
  const std::vector<std::int16_t> a{1};
  const std::vector<std::int16_t> b{1};
  std::vector<std::int32_t> c{0};
  try {
    MatrixArchitectureState invalid({0, 1, 1, 1, 1, 1}, {2, 2, 4}, a, b, c);
    (void)invalid;
    throw std::runtime_error("zero M dimension must be rejected");
  } catch (const std::invalid_argument&) {
  }
  try {
    MatrixArchitectureState invalid({1, 1, 1, 0, 1, 1}, {2, 2, 4}, a, b, c);
    (void)invalid;
    throw std::runtime_error("zero tile dimension must be rejected");
  } catch (const std::invalid_argument&) {
  }
  try {
    MatrixArchitectureState invalid({2, 1, 1, 1, 1, 1}, {2, 2, 4}, a, b, c);
    (void)invalid;
    throw std::runtime_error("incorrect A span length must be rejected");
  } catch (const std::length_error&) {
  }
  try {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    MatrixArchitectureState invalid({max, 1, 2, 1, 1, 1}, {2, 2, 4}, a, b, c);
    (void)invalid;
    throw std::runtime_error("overflowing tensor element count must be rejected");
  } catch (const std::overflow_error&) {
  }
}

void test_invalid_opcode_and_sequence_are_atomic() {
  const MatrixWorkShape shape{1, 1, 1, 1, 1, 1};
  const std::vector<std::int16_t> a{1};
  const std::vector<std::int16_t> b{2};
  std::vector<std::int32_t> c(1, -1);
  MatrixArchitectureState state(shape, capacities_for(shape), a, b, c);
  MatrixCommand bad_sequence{3, MatrixOpcode::clear_accumulator, 0, 0, 0, 1, 1, 0};
  require_error(state, bad_sequence, MatrixCommandError::sequence_mismatch,
                "wrong sequence ID is rejected first");
  MatrixCommand unknown = bad_sequence;
  unknown.sequence_id = 0;
  unknown.opcode = static_cast<MatrixOpcode>(255);
  require_error(state, unknown, MatrixCommandError::unexpected_opcode,
                "unknown opcode is rejected");
  require(state.next_sequence_id() == 0 && c[0] == -1,
          "invalid sequence and opcode leave state unchanged");
}

}  // namespace

int main() {
  try {
    test_scalar_oracle_shapes_and_k_panels();
    test_5x7x3_clipped_extents_and_row_major_offsets();
    test_modulo_overflow_across_k_panels();
    test_deterministic_trace_order_and_sequence_ids();
    test_canonical_unused_fields_and_retry();
    test_rejected_command_failure_atomicity();
    test_wrong_tile_and_wrong_panel_do_not_consume_sequence();
    test_invalid_indices_and_clipped_extents_are_rejected();
    test_capacity_boundaries_are_atomic();
    test_invalid_context_and_checked_sizes();
    test_invalid_opcode_and_sequence_are_atomic();
    std::cout << "matrix architecture contract tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "matrix architecture contract test failed: " << error.what() << '\n';
    return 1;
  }
}
