#pragma once

#include <cstdint>

namespace axissim {

// M6.3a models a shape-only, batch-one GEMM. It does not consume matrix
// values or produce a functional output tensor.
enum class GemmInputFormat {
  signed_i16_twos_complement,
};

enum class GemmAccumulatorOutputFormat {
  signed_i32_twos_complement,
};

enum class GemmLayout {
  row_major_contiguous,
};

enum class GemmOverflow {
  wrap_modulo_2pow32,
};

enum class GemmRounding {
  none,
};

enum class GemmInitialC {
  zero,
};

struct GemmWorkload {
  std::uint64_t m = 0;
  std::uint64_t n = 0;
  std::uint64_t k = 0;
  GemmInputFormat input_format = GemmInputFormat::signed_i16_twos_complement;
  GemmAccumulatorOutputFormat accumulator_output_format =
      GemmAccumulatorOutputFormat::signed_i32_twos_complement;
  GemmLayout layout = GemmLayout::row_major_contiguous;
  GemmOverflow overflow = GemmOverflow::wrap_modulo_2pow32;
  GemmRounding rounding = GemmRounding::none;
  GemmInitialC initial_c = GemmInitialC::zero;
};

struct GemmWorkCounts {
  std::uint64_t useful_macs = 0;
  std::uint64_t conventional_operation_equivalents = 0;
  std::uint64_t tensor_a_bytes = 0;
  std::uint64_t tensor_b_bytes = 0;
  std::uint64_t tensor_c_bytes = 0;
  std::uint64_t unique_tensor_payload_bytes = 0;
};

// Throws std::invalid_argument for unsupported semantics or zero dimensions.
void validate_gemm_workload(const GemmWorkload& workload);

// Counts use checked uint64 arithmetic and throw std::overflow_error if any
// work or logical payload count cannot be represented.
[[nodiscard]] GemmWorkCounts count_gemm_work(const GemmWorkload& workload);

}  // namespace axissim
