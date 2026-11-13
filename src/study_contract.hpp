#pragma once

#include <optional>
#include <string>

namespace axissim {

// Executes one validated v1 candidate request. Invalid requests are written as
// structured result records and return non-zero, as required by the study API.
int run_study_evaluate(const std::string& request_path, const std::string& result_path,
                       const std::string& executable_path,
                       const std::optional<std::string>& trace_path = std::nullopt);

}  // namespace axissim
