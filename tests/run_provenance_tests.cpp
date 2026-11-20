#include "run_provenance.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "build_info.hpp"
#include "gemm_json.hpp"
#include "sha256.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void write(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
  if (!output) throw std::runtime_error("unable to write test fixture");
}

const axissim::gemm_json::Value& field(const axissim::gemm_json::Value& value,
                                       const std::string& key) {
  return value.object().at(key);
}

}  // namespace

int main() {
  try {
    using namespace axissim;
    require(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 must match independently published abc fixture");

    const auto root = std::filesystem::temp_directory_path() / "axissim-run-provenance-contract";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "configs");
    const auto manifest = root / "configs" / "manifest.conf";
    const auto included = root / "configs" / "architecture.conf";
    const auto executable = root / "axissim-test-executable";
    write(manifest, "include = architecture.conf\nscenario_name = provenance_fixture\n");
    write(included, "frames = 1\nnpu_macs = 1024\n");
    const auto arrival_config = root / "configs" / "arrivals.conf";
    const auto arrival_trace = root / "configs" / "arrivals.csv";
    const auto arrival_provenance = root / "configs" / "arrivals.md";
    write(arrival_config,
          "scenario_name = trace_provenance_fixture\n"
          "frame_arrival_mode = trace\n"
          "frame_arrival_trace = arrivals.csv\n"
          "frame_arrival_provenance = arrivals.md\n"
          "frames = 1\n");
    write(arrival_trace, "release_us\n0\n");
    write(arrival_provenance, "synthetic test trace fixture\n");
    write(executable, "abc");

    const auto first_config = load_config_with_sources(manifest.string());
    const auto first = make_run_provenance(first_config, manifest, executable);
    const auto first_canonical = canonical_provenance_value(first);
    const auto repeated = make_run_provenance(first_config, manifest, executable);
    require(first_canonical == canonical_provenance_value(repeated),
            "identical executable and input records must have identical canonical provenance");
    const auto expected_identity = sha256_hex(canonical_provenance_value(field(first, "provenance")));
    require(field(first, "identity_sha256").string() == expected_identity,
            "identity hash must cover the canonical provenance payload");
    const auto& payload = field(first, "provenance");
    require(field(field(payload, "executable"), "sha256").string() == sha256_hex("abc"),
            "the executable record must hash the actual executable bytes");
    require(field(field(payload, "manifest"), "identity").object().at("sha256").string() == sha256_hex("include = architecture.conf\nscenario_name = provenance_fixture\n"),
            "the root manifest hash must use the exact bytes parsed by the configuration loader");
    const auto expected_build_status =
        AXISSIM_SOURCE_REVISION[0] == '\0' || std::string(AXISSIM_SOURCE_REVISION) == "unknown" ||
                std::string(AXISSIM_SOURCE_TREE_STATE) != "clean" ||
                std::string(AXISSIM_COMPILER_ID) == "unknown" ||
                std::string(AXISSIM_COMPILER_VERSION) == "unknown"
            ? "incomplete"
            : "complete";
    require(field(first, "identity_status").string() == expected_build_status,
            "identity status must reflect embedded build identity completeness");

    const auto arrival_record = make_run_provenance(load_config_with_sources(arrival_config.string()),
                                                     arrival_config, executable);
    const auto& arrival_payload = field(arrival_record, "provenance");
    const auto& arrival_files = field(arrival_payload, "configuration_files").array();
    bool found_arrival_trace = false;
    bool found_arrival_provenance = false;
    for (const auto& input : arrival_files) {
      require(input.object().at("path").string().find(root.string()) == std::string::npos,
              "provenance paths must not expose absolute checkout paths");
      if (input.object().at("role").string() == "frame_arrival_trace") {
        found_arrival_trace = input.object().at("sha256").string() == sha256_hex("release_us\n0\n");
      }
      if (input.object().at("role").string() == "frame_arrival_provenance") {
        found_arrival_provenance = input.object().at("sha256").string() == sha256_hex("synthetic test trace fixture\n");
      }
    }
    require(found_arrival_trace && found_arrival_provenance,
            "trace and its provenance source must both be hashed as resolved inputs");
    const auto& resolved = field(arrival_payload, "configuration");
    require(resolved.object().at("frame_arrival_trace").string() == "arrivals.csv" &&
                resolved.object().at("frame_arrival_provenance").string() == "arrivals.md",
            "resolved trace paths must match the manifest-relative files parsed by the simulator");

    const auto original_current_path = std::filesystem::current_path();
    std::filesystem::current_path(root);
    const auto relative_manifest = std::filesystem::path("configs/arrivals.conf");
    const auto relative_loaded = load_config_with_sources(relative_manifest.string());
    const auto relative_record = make_run_provenance(relative_loaded, relative_manifest, executable);
    std::filesystem::current_path(original_current_path);
    require(canonical_provenance_value(relative_record) == canonical_provenance_value(arrival_record),
            "relative and absolute manifest invocations must identify the same resolved inputs");
    write(arrival_trace, "release_us\n0\n1\n");
    const auto trace_item = std::find_if(arrival_files.begin(), arrival_files.end(), [](const auto& input) {
      return input.object().at("role").string() == "frame_arrival_trace";
    });
    require(trace_item != arrival_files.end() &&
                trace_item->object().at("sha256").string() == sha256_hex("release_us\n0\n"),
            "a frozen provenance snapshot must retain the input hash captured before execution");
    require(canonical_provenance_value(arrival_record) != canonical_provenance_value(
                make_run_provenance(load_config_with_sources(arrival_config.string()), arrival_config, executable)),
            "a changed referenced runtime input must produce a new identity for a later run");

    const auto overridden = load_config_with_sources(manifest.string(), {{"npu_macs", "512"}});
    const auto override_record = make_run_provenance(overridden, manifest, executable);
    require(canonical_provenance_value(override_record) != first_canonical,
            "explicit CLI overrides must affect provenance identity");
    require(field(field(override_record, "provenance"), "explicit_cli_overrides").object().at("npu_macs").string() == "512",
            "the explicit override must be recorded separately");

    write(included, "frames = 1\nnpu_macs = 2048\n");
    const auto modified_include = load_config_with_sources(manifest.string());
    const auto include_record = make_run_provenance(modified_include, manifest, executable);
    require(canonical_provenance_value(include_record) != first_canonical,
            "a changed transitive configuration input must change provenance identity");

    const auto missing_executable = make_run_provenance(modified_include, manifest, root / "missing-executable");
    require(field(missing_executable, "identity_status").string() == "incomplete",
            "unavailable executable identity must be explicitly incomplete");
    require(field(missing_executable, "identity_sha256").string() != "unknown",
            "incomplete provenance still needs an integrity hash distinct from verified status");
    require(field(field(missing_executable, "provenance").object().at("executable"), "status").string() == "unavailable",
            "missing executable hash must not be presented as verified");

    std::filesystem::remove_all(root);
    std::cout << "run provenance deterministic hash, override, transitive-input, and unavailable-identity contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "run provenance test failure: " << error.what() << '\n';
    return 1;
  }
}
