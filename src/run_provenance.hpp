#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "gemm_json.hpp"

namespace axissim {

// Builds an opt-in sidecar record for a completed CLI run. The simulator's
// result JSON remains the numerical contract; this record binds that run to
// the exact resolved inputs and build/executable identities.
[[nodiscard]] gemm_json::Value make_run_provenance(
    const ConfigLoadResult& loaded, const std::filesystem::path& root_manifest,
    const std::filesystem::path& executable);
[[nodiscard]] gemm_json::Value finalize_run_provenance(
    const gemm_json::Value& input_snapshot, const ConfigLoadResult& loaded,
    const std::filesystem::path& root_manifest,
    const std::filesystem::path& result_json_path);
[[nodiscard]] std::string canonical_provenance_value(const gemm_json::Value& value);
[[nodiscard]] std::string canonical_run_provenance(
    const ConfigLoadResult& loaded, const std::filesystem::path& root_manifest,
    const std::filesystem::path& executable);
void write_run_provenance(const std::filesystem::path& result_json_path,
                          const ConfigLoadResult& loaded,
                          const std::filesystem::path& root_manifest,
                          const std::filesystem::path& executable);
void write_run_provenance_record(const std::filesystem::path& result_json_path,
                                 const gemm_json::Value& provenance_record);

}  // namespace axissim
