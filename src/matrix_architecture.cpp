#include "matrix_architecture.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace axissim {
namespace {

static_assert(sizeof(std::int16_t) == 2);
static_assert(sizeof(std::int32_t) == 4);
static_assert(sizeof(std::uint32_t) == sizeof(std::int32_t));

[[nodiscard]] std::uint64_t checked_add(std::uint64_t left,
                                        std::uint64_t right,
                                        const char* what) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::overflow_error(std::string("matrix architecture overflow while calculating ") +
                              what);
  }
  return left + right;
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t left,
                                             std::uint64_t right,
                                             const char* what) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::overflow_error(std::string("matrix architecture overflow while calculating ") +
                              what);
  }
  return left * right;
}

[[nodiscard]] std::size_t checked_size(std::uint64_t value,
                                       const char* what) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error(std::string("matrix architecture size exceeds size_t for ") +
                              what);
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] std::uint64_t checked_bytes(std::uint64_t elements,
                                          std::uint64_t element_bytes,
                                          const char* what) {
  return checked_multiply(elements, element_bytes, what);
}

[[nodiscard]] std::uint64_t ceil_div(std::uint64_t value,
                                     std::uint64_t divisor) noexcept {
  return value / divisor + static_cast<std::uint64_t>(value % divisor != 0);
}

[[nodiscard]] bool is_known_opcode(MatrixOpcode opcode) noexcept {
  switch (opcode) {
    case MatrixOpcode::clear_accumulator:
    case MatrixOpcode::load_a:
    case MatrixOpcode::load_b:
    case MatrixOpcode::matrix_mac_accumulate:
    case MatrixOpcode::store_accumulator_tile:
      return true;
  }
  return false;
}

[[nodiscard]] bool same_tile(std::uint64_t left_m, std::uint64_t left_n,
                             std::uint64_t right_m,
                             std::uint64_t right_n) noexcept {
  return left_m == right_m && left_n == right_n;
}

}  // namespace

std::string_view matrix_command_error_name(MatrixCommandError error) noexcept {
  switch (error) {
    case MatrixCommandError::none: return "none";
    case MatrixCommandError::sequence_mismatch: return "sequence_mismatch";
    case MatrixCommandError::unexpected_opcode: return "unexpected_opcode";
    case MatrixCommandError::unused_field_nonzero: return "unused_field_nonzero";
    case MatrixCommandError::wrong_tile: return "wrong_tile";
    case MatrixCommandError::wrong_k_panel: return "wrong_k_panel";
    case MatrixCommandError::slot_busy: return "slot_busy";
    case MatrixCommandError::missing_operand: return "missing_operand";
    case MatrixCommandError::shape_mismatch: return "shape_mismatch";
    case MatrixCommandError::invalid_extent: return "invalid_extent";
    case MatrixCommandError::source_bounds: return "source_bounds";
    case MatrixCommandError::accumulator_not_complete:
      return "accumulator_not_complete";
    case MatrixCommandError::duplicate_store: return "duplicate_store";
    case MatrixCommandError::capacity_exceeded: return "capacity_exceeded";
    case MatrixCommandError::arithmetic_overflow: return "arithmetic_overflow";
  }
  return "unknown_error";
}

MatrixArchitectureState::MatrixArchitectureState(
    MatrixWorkShape shape, MatrixBankCapacities capacities,
    std::span<const std::int16_t> a, std::span<const std::int16_t> b,
    std::span<std::int32_t> c)
    : shape_(shape), capacities_(capacities), a_(a), b_(b), c_(c) {
  if (shape_.m == 0 || shape_.n == 0 || shape_.k == 0 ||
      shape_.tile_m == 0 || shape_.tile_n == 0 || shape_.tile_k == 0) {
    throw std::invalid_argument("matrix dimensions and tile extents must be positive");
  }

  const std::uint64_t a_elements = checked_multiply(shape_.m, shape_.k, "A tensor elements");
  const std::uint64_t b_elements = checked_multiply(shape_.k, shape_.n, "B tensor elements");
  const std::uint64_t c_elements = checked_multiply(shape_.m, shape_.n, "C tensor elements");
  const std::size_t a_size = checked_size(a_elements, "A tensor");
  const std::size_t b_size = checked_size(b_elements, "B tensor");
  const std::size_t c_size = checked_size(c_elements, "C tensor");
  if (a_.size() != a_size || b_.size() != b_size || c_.size() != c_size) {
    throw std::length_error("matrix tensor spans must exactly match MxK, KxN, and MxN");
  }
}

MatrixCommandError MatrixArchitectureState::derive_extent(
    std::uint64_t tile_m_index, std::uint64_t tile_n_index, bool needs_k,
    std::uint64_t k_panel_index, TileExtent& extent) const {
  try {
    const std::uint64_t tiles_m = ceil_div(shape_.m, shape_.tile_m);
    const std::uint64_t tiles_n = ceil_div(shape_.n, shape_.tile_n);
    if (tile_m_index >= tiles_m || tile_n_index >= tiles_n) {
      return MatrixCommandError::invalid_extent;
    }
    extent.m0 = checked_multiply(tile_m_index, shape_.tile_m, "M tile offset");
    extent.n0 = checked_multiply(tile_n_index, shape_.tile_n, "N tile offset");
    extent.valid_m = std::min(shape_.tile_m, shape_.m - extent.m0);
    extent.valid_n = std::min(shape_.tile_n, shape_.n - extent.n0);
    extent.k0 = 0;
    extent.valid_k = 0;
    if (needs_k) {
      const std::uint64_t tiles_k = ceil_div(shape_.k, shape_.tile_k);
      if (k_panel_index >= tiles_k) return MatrixCommandError::invalid_extent;
      extent.k0 = checked_multiply(k_panel_index, shape_.tile_k, "K panel offset");
      extent.valid_k = std::min(shape_.tile_k, shape_.k - extent.k0);
    }
  } catch (const std::overflow_error&) {
    return MatrixCommandError::arithmetic_overflow;
  }
  return MatrixCommandError::none;
}

MatrixCommandError MatrixArchitectureState::validate_unused_fields(
    const MatrixCommand& command) const noexcept {
  switch (command.opcode) {
    case MatrixOpcode::clear_accumulator:
    case MatrixOpcode::store_accumulator_tile:
      return (command.k_panel_index != 0 || command.valid_k != 0)
                 ? MatrixCommandError::unused_field_nonzero
                 : MatrixCommandError::none;
    case MatrixOpcode::load_a:
      return command.valid_n != 0 ? MatrixCommandError::unused_field_nonzero
                                  : MatrixCommandError::none;
    case MatrixOpcode::load_b:
      return command.valid_m != 0 ? MatrixCommandError::unused_field_nonzero
                                  : MatrixCommandError::none;
    case MatrixOpcode::matrix_mac_accumulate:
      return MatrixCommandError::none;
  }
  return MatrixCommandError::unexpected_opcode;
}

MatrixCommandError MatrixArchitectureState::validate_command_state(
    const MatrixCommand& command, const TileExtent& extent) const noexcept {
  switch (command.opcode) {
    case MatrixOpcode::clear_accumulator:
      if (accumulator_active_ || a_slot_.loaded || b_slot_.loaded) {
        return MatrixCommandError::slot_busy;
      }
      return complete_ ? MatrixCommandError::unexpected_opcode
                       : MatrixCommandError::none;
    case MatrixOpcode::load_a:
      if (!accumulator_active_) return MatrixCommandError::unexpected_opcode;
      if (accumulator_ready_) return MatrixCommandError::accumulator_not_complete;
      if (a_slot_.loaded) return MatrixCommandError::slot_busy;
      if (b_slot_.loaded) return MatrixCommandError::unexpected_opcode;
      return MatrixCommandError::none;
    case MatrixOpcode::load_b:
      if (!accumulator_active_) return MatrixCommandError::unexpected_opcode;
      if (accumulator_ready_) return MatrixCommandError::accumulator_not_complete;
      if (!a_slot_.loaded) return MatrixCommandError::missing_operand;
      if (b_slot_.loaded) return MatrixCommandError::slot_busy;
      return MatrixCommandError::none;
    case MatrixOpcode::matrix_mac_accumulate:
      if (!accumulator_active_) return MatrixCommandError::unexpected_opcode;
      if (accumulator_ready_) return MatrixCommandError::accumulator_not_complete;
      if (!a_slot_.loaded || !b_slot_.loaded) {
        return MatrixCommandError::missing_operand;
      }
      if (!same_tile(a_slot_.tile_m_index, a_slot_.tile_n_index,
                     accumulator_tile_m_index_, accumulator_tile_n_index_) ||
          !same_tile(b_slot_.tile_m_index, b_slot_.tile_n_index,
                     accumulator_tile_m_index_, accumulator_tile_n_index_) ||
          a_slot_.k_panel_index != next_k_panel_index_ ||
          b_slot_.k_panel_index != next_k_panel_index_ ||
          a_slot_.rows != extent.valid_m ||
          a_slot_.columns != extent.valid_k ||
          b_slot_.rows != extent.valid_k ||
          b_slot_.columns != extent.valid_n) {
        return MatrixCommandError::shape_mismatch;
      }
      return MatrixCommandError::none;
    case MatrixOpcode::store_accumulator_tile:
      if (!accumulator_active_) {
        if (has_last_stored_tile_ &&
            same_tile(command.tile_m_index, command.tile_n_index,
                      last_stored_tile_m_index_, last_stored_tile_n_index_)) {
          return MatrixCommandError::duplicate_store;
        }
        return MatrixCommandError::unexpected_opcode;
      }
      if (!accumulator_ready_) return MatrixCommandError::accumulator_not_complete;
      if (a_slot_.loaded || b_slot_.loaded) return MatrixCommandError::missing_operand;
      return MatrixCommandError::none;
  }
  return MatrixCommandError::unexpected_opcode;
}

MatrixCommandError MatrixArchitectureState::validate_extents(
    const MatrixCommand& command, const TileExtent& extent) const noexcept {
  switch (command.opcode) {
    case MatrixOpcode::clear_accumulator:
    case MatrixOpcode::store_accumulator_tile:
      return command.valid_m == extent.valid_m &&
                     command.valid_n == extent.valid_n && command.valid_k == 0
                 ? MatrixCommandError::none
                 : MatrixCommandError::invalid_extent;
    case MatrixOpcode::load_a:
      return command.valid_m == extent.valid_m &&
                     command.valid_k == extent.valid_k && command.valid_n == 0
                 ? MatrixCommandError::none
                 : MatrixCommandError::invalid_extent;
    case MatrixOpcode::load_b:
      return command.valid_n == extent.valid_n &&
                     command.valid_k == extent.valid_k && command.valid_m == 0
                 ? MatrixCommandError::none
                 : MatrixCommandError::invalid_extent;
    case MatrixOpcode::matrix_mac_accumulate:
      return command.valid_m == extent.valid_m &&
                     command.valid_n == extent.valid_n &&
                     command.valid_k == extent.valid_k
                 ? MatrixCommandError::none
                 : MatrixCommandError::invalid_extent;
  }
  return MatrixCommandError::unexpected_opcode;
}

MatrixCommandError MatrixArchitectureState::validate_store_bounds(
    const TileExtent& extent) const noexcept {
  try {
    for (std::uint64_t row = 0; row < extent.valid_m; ++row) {
      const std::uint64_t global_row = checked_add(extent.m0, row, "C row");
      const std::uint64_t start = checked_add(
          checked_multiply(global_row, shape_.n, "C row offset"), extent.n0,
          "C tile offset");
      const std::uint64_t end = checked_add(start, extent.valid_n, "C tile end");
      if (end > c_.size()) return MatrixCommandError::source_bounds;
    }
  } catch (const std::overflow_error&) {
    return MatrixCommandError::arithmetic_overflow;
  }
  return MatrixCommandError::none;
}

MatrixCommandError MatrixArchitectureState::validate_load_a_bounds(
    const TileExtent& extent) const noexcept {
  try {
    for (std::uint64_t row = 0; row < extent.valid_m; ++row) {
      const std::uint64_t global_row = checked_add(extent.m0, row, "A row");
      const std::uint64_t start = checked_add(
          checked_multiply(global_row, shape_.k, "A row offset"), extent.k0,
          "A tile offset");
      const std::uint64_t end = checked_add(start, extent.valid_k, "A tile end");
      if (end > a_.size()) return MatrixCommandError::source_bounds;
    }
  } catch (const std::overflow_error&) {
    return MatrixCommandError::arithmetic_overflow;
  }
  return MatrixCommandError::none;
}

MatrixCommandError MatrixArchitectureState::validate_load_b_bounds(
    const TileExtent& extent) const noexcept {
  try {
    for (std::uint64_t row = 0; row < extent.valid_k; ++row) {
      const std::uint64_t global_row = checked_add(extent.k0, row, "B row");
      const std::uint64_t start = checked_add(
          checked_multiply(global_row, shape_.n, "B row offset"), extent.n0,
          "B tile offset");
      const std::uint64_t end = checked_add(start, extent.valid_n, "B tile end");
      if (end > b_.size()) return MatrixCommandError::source_bounds;
    }
  } catch (const std::overflow_error&) {
    return MatrixCommandError::arithmetic_overflow;
  }
  return MatrixCommandError::none;
}

MatrixCommandResult MatrixArchitectureState::apply(const MatrixCommand& command) {
  if (command.sequence_id != next_sequence_id_) {
    return {MatrixCommandError::sequence_mismatch};
  }
  if (!is_known_opcode(command.opcode)) {
    return {MatrixCommandError::unexpected_opcode};
  }

  MatrixCommandError error = validate_unused_fields(command);
  if (error != MatrixCommandError::none) return {error};

  TileExtent extent;
  const bool needs_k = command.opcode == MatrixOpcode::load_a ||
                       command.opcode == MatrixOpcode::load_b ||
                       command.opcode == MatrixOpcode::matrix_mac_accumulate;
  error = derive_extent(command.tile_m_index, command.tile_n_index, needs_k,
                        command.k_panel_index, extent);
  if (error != MatrixCommandError::none) return {error};

  if (command.opcode == MatrixOpcode::store_accumulator_tile &&
      !accumulator_active_ && has_last_stored_tile_ &&
      same_tile(command.tile_m_index, command.tile_n_index,
                last_stored_tile_m_index_, last_stored_tile_n_index_)) {
    return {MatrixCommandError::duplicate_store};
  }

  const std::uint64_t expected_m = accumulator_active_
                                       ? accumulator_tile_m_index_
                                       : next_tile_m_index_;
  const std::uint64_t expected_n = accumulator_active_
                                       ? accumulator_tile_n_index_
                                       : next_tile_n_index_;
  if (!same_tile(command.tile_m_index, command.tile_n_index,
                 expected_m, expected_n)) {
    return {MatrixCommandError::wrong_tile};
  }
  if (needs_k && command.k_panel_index != next_k_panel_index_) {
    return {MatrixCommandError::wrong_k_panel};
  }

  error = validate_command_state(command, extent);
  if (error != MatrixCommandError::none) return {error};
  error = validate_extents(command, extent);
  if (error != MatrixCommandError::none) return {error};

  std::uint64_t a_elements = 0;
  std::uint64_t b_elements = 0;
  std::uint64_t accumulator_elements = 0;
  try {
    a_elements = checked_multiply(extent.valid_m, extent.valid_k, "A tile elements");
    b_elements = checked_multiply(extent.valid_k, extent.valid_n, "B tile elements");
    accumulator_elements = checked_multiply(extent.valid_m, extent.valid_n,
                                            "accumulator tile elements");
  } catch (const std::overflow_error&) {
    return {MatrixCommandError::arithmetic_overflow};
  }

  switch (command.opcode) {
    case MatrixOpcode::clear_accumulator:
      try {
        const std::uint64_t bytes = checked_bytes(accumulator_elements, 4,
                                                  "accumulator tile bytes");
        if (bytes > capacities_.accumulator_bytes) {
          return {MatrixCommandError::capacity_exceeded};
        }
      } catch (const std::overflow_error&) {
        return {MatrixCommandError::arithmetic_overflow};
      }
      break;
    case MatrixOpcode::load_a:
      error = validate_load_a_bounds(extent);
      if (error != MatrixCommandError::none) return {error};
      try {
        if (checked_bytes(a_elements, 2, "A tile bytes") > capacities_.a_bytes) {
          return {MatrixCommandError::capacity_exceeded};
        }
      } catch (const std::overflow_error&) {
        return {MatrixCommandError::arithmetic_overflow};
      }
      break;
    case MatrixOpcode::load_b:
      error = validate_load_b_bounds(extent);
      if (error != MatrixCommandError::none) return {error};
      try {
        if (checked_bytes(b_elements, 2, "B tile bytes") > capacities_.b_bytes) {
          return {MatrixCommandError::capacity_exceeded};
        }
      } catch (const std::overflow_error&) {
        return {MatrixCommandError::arithmetic_overflow};
      }
      break;
    case MatrixOpcode::matrix_mac_accumulate:
      break;
    case MatrixOpcode::store_accumulator_tile:
      error = validate_store_bounds(extent);
      if (error != MatrixCommandError::none) return {error};
      break;
  }

  // Reaching UINT64_MAX would make the next command ID unrepresentable. Reject
  // before any command-side mutation, as required by the sequence contract.
  if (next_sequence_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return {MatrixCommandError::arithmetic_overflow};
  }

  switch (command.opcode) {
    case MatrixOpcode::clear_accumulator: {
      std::vector<std::int32_t> next_accumulator(
          checked_size(accumulator_elements, "accumulator tile"), 0);
      accumulator_.swap(next_accumulator);
      accumulator_active_ = true;
      accumulator_ready_ = false;
      accumulator_tile_m_index_ = command.tile_m_index;
      accumulator_tile_n_index_ = command.tile_n_index;
      next_k_panel_index_ = 0;
      ++next_sequence_id_;
      return {};
    }
    case MatrixOpcode::load_a: {
      std::vector<std::int16_t> next_values(checked_size(a_elements, "A tile"));
      for (std::uint64_t row = 0; row < extent.valid_m; ++row) {
        const std::uint64_t source =
            (extent.m0 + row) * shape_.k + extent.k0;
        const std::uint64_t destination = row * extent.valid_k;
        for (std::uint64_t column = 0; column < extent.valid_k; ++column) {
          next_values[static_cast<std::size_t>(destination + column)] =
              a_[static_cast<std::size_t>(source + column)];
        }
      }
      a_slot_.values.swap(next_values);
      a_slot_.loaded = true;
      a_slot_.tile_m_index = command.tile_m_index;
      a_slot_.tile_n_index = command.tile_n_index;
      a_slot_.k_panel_index = command.k_panel_index;
      a_slot_.rows = extent.valid_m;
      a_slot_.columns = extent.valid_k;
      ++next_sequence_id_;
      return {};
    }
    case MatrixOpcode::load_b: {
      std::vector<std::int16_t> next_values(checked_size(b_elements, "B tile"));
      for (std::uint64_t row = 0; row < extent.valid_k; ++row) {
        const std::uint64_t source = (extent.k0 + row) * shape_.n + extent.n0;
        const std::uint64_t destination = row * extent.valid_n;
        for (std::uint64_t column = 0; column < extent.valid_n; ++column) {
          next_values[static_cast<std::size_t>(destination + column)] =
              b_[static_cast<std::size_t>(source + column)];
        }
      }
      b_slot_.values.swap(next_values);
      b_slot_.loaded = true;
      b_slot_.tile_m_index = command.tile_m_index;
      b_slot_.tile_n_index = command.tile_n_index;
      b_slot_.k_panel_index = command.k_panel_index;
      b_slot_.rows = extent.valid_k;
      b_slot_.columns = extent.valid_n;
      ++next_sequence_id_;
      return {};
    }
    case MatrixOpcode::matrix_mac_accumulate: {
      std::vector<std::int32_t> next_accumulator = accumulator_;
      for (std::uint64_t row = 0; row < extent.valid_m; ++row) {
        for (std::uint64_t column = 0; column < extent.valid_n; ++column) {
          const std::size_t accumulator_index = static_cast<std::size_t>(
              row * extent.valid_n + column);
          std::uint32_t bits = std::bit_cast<std::uint32_t>(
              next_accumulator[accumulator_index]);
          for (std::uint64_t inner = 0; inner < extent.valid_k; ++inner) {
            const std::int64_t product =
                static_cast<std::int64_t>(a_slot_.values[static_cast<std::size_t>(
                    row * extent.valid_k + inner)]) *
                static_cast<std::int64_t>(b_slot_.values[static_cast<std::size_t>(
                    inner * extent.valid_n + column)]);
            bits += static_cast<std::uint32_t>(static_cast<std::int32_t>(product));
          }
          next_accumulator[accumulator_index] = std::bit_cast<std::int32_t>(bits);
        }
      }
      accumulator_.swap(next_accumulator);
      a_slot_.values.clear();
      b_slot_.values.clear();
      a_slot_.loaded = false;
      b_slot_.loaded = false;
      ++next_k_panel_index_;
      if (next_k_panel_index_ == ceil_div(shape_.k, shape_.tile_k)) {
        accumulator_ready_ = true;
      }
      ++next_sequence_id_;
      return {};
    }
    case MatrixOpcode::store_accumulator_tile: {
      std::vector<std::size_t> output_offsets;
      output_offsets.reserve(checked_size(extent.valid_m, "C tile row offsets"));
      for (std::uint64_t row = 0; row < extent.valid_m; ++row) {
        const std::uint64_t destination =
            (extent.m0 + row) * shape_.n + extent.n0;
        output_offsets.push_back(checked_size(destination, "C tile row offset"));
      }
      for (std::uint64_t row = 0; row < extent.valid_m; ++row) {
        const std::size_t source = static_cast<std::size_t>(row * extent.valid_n);
        const std::size_t destination = output_offsets[static_cast<std::size_t>(row)];
        for (std::uint64_t column = 0; column < extent.valid_n; ++column) {
          c_[destination + static_cast<std::size_t>(column)] =
              accumulator_[source + static_cast<std::size_t>(column)];
        }
      }
      last_stored_tile_m_index_ = command.tile_m_index;
      last_stored_tile_n_index_ = command.tile_n_index;
      has_last_stored_tile_ = true;
      accumulator_.clear();
      accumulator_active_ = false;
      accumulator_ready_ = false;
      next_k_panel_index_ = 0;
      if (++next_tile_n_index_ == ceil_div(shape_.n, shape_.tile_n)) {
        next_tile_n_index_ = 0;
        ++next_tile_m_index_;
      }
      complete_ = next_tile_m_index_ == ceil_div(shape_.m, shape_.tile_m);
      ++next_sequence_id_;
      return {};
    }
  }
  return {MatrixCommandError::unexpected_opcode};
}

}  // namespace axissim
