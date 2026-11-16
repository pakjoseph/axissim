#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace axissim {

struct GemmPairResponse {
  std::string json;
  int exit_code = 4;
};

[[nodiscard]] GemmPairResponse
evaluate_study_gemm_pair(std::string_view request_json,
                         const std::filesystem::path &executable_path = {});

int run_study_gemm_pair(const std::string &request_path,
                        const std::string &result_path,
                        const std::filesystem::path &executable_path);

} // namespace axissim
