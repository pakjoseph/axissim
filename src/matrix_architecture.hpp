#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace axissim {

enum class MatrixOpcode : std::uint8_t {
  clear_accumulator,
  load_a,
  load_b,
  matrix_mac_accumulate,
  store_accumulator_tile,
};

// Fixed-field internal command record. Fields unused by an opcode must be zero.
// The record carries identities and valid extents only; it contains no pointers.
struct MatrixCommand {
  std::uint64_t sequence_id = 0;
  MatrixOpcode opcode = MatrixOpcode::clear_accumulator;
  std::uint64_t tile_m_index = 0;
  std::uint64_t tile_n_index = 0;
  std::uint64_t k_panel_index = 0;
  std::uint64_t valid_m = 0;
  std::uint64_t valid_n = 0;
  std::uint64_t valid_k = 0;

  friend constexpr bool operator==(const MatrixCommand&,
                                   const MatrixCommand&) = default;
};

struct MatrixWorkShape {
  std::uint64_t m = 0;
  std::uint64_t n = 0;
  std::uint64_t k = 0;
  std::uint64_t tile_m = 0;
  std::uint64_t tile_n = 0;
  std::uint64_t tile_k = 0;
};

struct MatrixBankCapacities {
  std::uint64_t a_bytes = 0;
  std::uint64_t b_bytes = 0;
  std::uint64_t accumulator_bytes = 0;
};

enum class MatrixCommandError : std::uint8_t {
  none,
  sequence_mismatch,
  unexpected_opcode,
  unused_field_nonzero,
  wrong_tile,
  wrong_k_panel,
  slot_busy,
  missing_operand,
  shape_mismatch,
  invalid_extent,
  source_bounds,
  accumulator_not_complete,
  duplicate_store,
  capacity_exceeded,
  arithmetic_overflow,
};

struct MatrixCommandResult {
  MatrixCommandError error = MatrixCommandError::none;

  [[nodiscard]] bool accepted() const noexcept {
    return error == MatrixCommandError::none;
  }
};

[[nodiscard]] std::string_view matrix_command_error_name(
    MatrixCommandError error) noexcept;

// Stateful functional command model for row-major signed-i16 GEMM into
// signed-i32 output. Accumulation wraps modulo 2^32. This is not a timing,
// AME-ISA, memory-system, power, or silicon model.
//
// Construction validates positive dimensions/tile extents and exact tensor
// span lengths. It throws std::invalid_argument for zero dimensions or invalid
// extents, std::overflow_error for unrepresentable products, and
// std::length_error when a span length does not match its tensor shape.
class MatrixArchitectureState {
 public:
  MatrixArchitectureState(MatrixWorkShape shape,
                         MatrixBankCapacities capacities,
                         std::span<const std::int16_t> a,
                         std::span<const std::int16_t> b,
                         std::span<std::int32_t> c);

  // A rejected command leaves both operand slots, accumulator, output tensor,
  // and next_sequence_id() unchanged. A corrected command may reuse that ID.
  [[nodiscard]] MatrixCommandResult apply(const MatrixCommand& command);

  [[nodiscard]] bool complete() const noexcept { return complete_; }
  [[nodiscard]] std::uint64_t next_sequence_id() const noexcept {
    return next_sequence_id_;
  }

 private:
  struct TileExtent {
    std::uint64_t m0 = 0;
    std::uint64_t n0 = 0;
    std::uint64_t k0 = 0;
    std::uint64_t valid_m = 0;
    std::uint64_t valid_n = 0;
    std::uint64_t valid_k = 0;
  };

  struct OperandSlot {
    bool loaded = false;
    std::uint64_t tile_m_index = 0;
    std::uint64_t tile_n_index = 0;
    std::uint64_t k_panel_index = 0;
    std::uint64_t rows = 0;
    std::uint64_t columns = 0;
    std::vector<std::int16_t> values;
  };

  [[nodiscard]] MatrixCommandError derive_extent(
      std::uint64_t tile_m_index, std::uint64_t tile_n_index,
      bool needs_k, std::uint64_t k_panel_index, TileExtent& extent) const;
  [[nodiscard]] MatrixCommandError validate_unused_fields(
      const MatrixCommand& command) const noexcept;
  [[nodiscard]] MatrixCommandError validate_command_state(
      const MatrixCommand& command, const TileExtent& extent) const noexcept;
  [[nodiscard]] MatrixCommandError validate_extents(
      const MatrixCommand& command, const TileExtent& extent) const noexcept;
  [[nodiscard]] MatrixCommandError validate_store_bounds(
      const TileExtent& extent) const noexcept;
  [[nodiscard]] MatrixCommandError validate_load_a_bounds(
      const TileExtent& extent) const noexcept;
  [[nodiscard]] MatrixCommandError validate_load_b_bounds(
      const TileExtent& extent) const noexcept;

  MatrixWorkShape shape_;
  MatrixBankCapacities capacities_;
  std::span<const std::int16_t> a_;
  std::span<const std::int16_t> b_;
  std::span<std::int32_t> c_;
  OperandSlot a_slot_;
  OperandSlot b_slot_;
  std::vector<std::int32_t> accumulator_;
  bool accumulator_active_ = false;
  bool accumulator_ready_ = false;
  std::uint64_t accumulator_tile_m_index_ = 0;
  std::uint64_t accumulator_tile_n_index_ = 0;
  std::uint64_t next_k_panel_index_ = 0;
  std::uint64_t next_tile_m_index_ = 0;
  std::uint64_t next_tile_n_index_ = 0;
  std::uint64_t last_stored_tile_m_index_ = 0;
  std::uint64_t last_stored_tile_n_index_ = 0;
  bool has_last_stored_tile_ = false;
  std::uint64_t next_sequence_id_ = 0;
  bool complete_ = false;
};

}  // namespace axissim
