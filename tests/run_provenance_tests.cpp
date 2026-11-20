#include "run_provenance.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include "build_info.hpp"
#include "gemm_json.hpp"
#include "simulation.hpp"
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
                                       std::string_view key) {
  const auto& object = value.object();
  const auto found = object.find(std::string(key));
  if (found == object.end()) throw std::out_of_range("missing JSON test field");
  return found->second;
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
    write(arrival_trace, "frame,release_us\n0,0\n");
    write(arrival_provenance, "synthetic test trace fixture\n");
    write(executable, "abc");

    const auto first_config = load_config_with_sources(manifest.string());
    const auto first_inputs = capture_runtime_inputs(first_config.config);
    const auto first = make_run_provenance(first_config, manifest, executable, first_inputs);
    const auto first_canonical = canonical_provenance_value(first);
    const auto repeated = make_run_provenance(first_config, manifest, executable, first_inputs);
    require(first_canonical == canonical_provenance_value(repeated),
            "identical executable and input records must have identical canonical provenance");
    const auto identity_input = gemm_json::Value(gemm_json::Object{
        {"identity_status", field(first, "identity_status")},
        {"provenance", field(first, "provenance")}});
    const auto expected_identity = sha256_hex(canonical_provenance_value(identity_input));
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
    auto forged_complete = first;
    auto& forged_fields = std::get<gemm_json::Object>(forged_complete.storage);
    forged_fields["identity_status"] = gemm_json::Value("complete");
    auto& forged_payload = std::get<gemm_json::Object>(forged_fields.at("provenance").storage);
    auto& forged_source = std::get<gemm_json::Object>(forged_payload.at("source").storage);
    forged_source["revision"] = gemm_json::Value("unknown");
    const auto completeness_issue = run_provenance_completeness_issue(forged_complete);
    require(completeness_issue && completeness_issue->find("source revision") != std::string::npos,
            "a self-consistent status claim with unavailable source identity must not be treated as complete");

    const auto arrival_loaded = load_config_with_sources(arrival_config.string());
    const auto arrival_captured = capture_runtime_inputs(arrival_loaded.config);
    const auto arrival_prepared = prepare_run_inputs(arrival_loaded.config, arrival_captured);
    const auto arrival_record = make_run_provenance(arrival_loaded, arrival_config, executable, arrival_captured);
    const auto& arrival_payload = field(arrival_record, "provenance");
    const auto& arrival_files = field(arrival_payload, "configuration_files").array();
    bool found_arrival_trace = false;
    bool found_arrival_provenance = false;
    for (const auto& input : arrival_files) {
      require(input.object().at("path").string().find(root.string()) == std::string::npos,
              "provenance paths must not expose absolute checkout paths");
      if (input.object().at("role").string() == "frame_arrival_trace") {
        found_arrival_trace = input.object().at("sha256").string() == sha256_hex("frame,release_us\n0,0\n");
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
    const auto relative_captured = capture_runtime_inputs(relative_loaded.config);
    const auto relative_record = make_run_provenance(relative_loaded, relative_manifest, executable, relative_captured);
    std::filesystem::current_path(original_current_path);
    require(canonical_provenance_value(relative_record) == canonical_provenance_value(arrival_record),
            "relative and absolute manifest invocations must identify the same resolved inputs");

    const auto result_file = root / "result.json";
    write(result_file, "{\"schema_version\":\"1.2\"}\n");
    const auto finalized = finalize_run_provenance(arrival_record, result_file);
    const auto& finalized_payload = field(finalized, "provenance");
    require(field(field(finalized_payload, "result"), "sha256").string() ==
                sha256_hex("{\"schema_version\":\"1.2\"}\n"),
            "final provenance must hash the exact emitted numerical result bytes");
    require(field(field(finalized_payload, "input_verification"), "status").string() ==
                "exact_bytes_captured_and_used",
            "provenance must state that exact captured bytes were passed to execution");

    auto make_complete_fixture = [](gemm_json::Value record) {
      auto& top = std::get<gemm_json::Object>(record.storage);
      top["identity_status"] = gemm_json::Value("complete");
      auto& payload = std::get<gemm_json::Object>(top.at("provenance").storage);
      auto& source = std::get<gemm_json::Object>(payload.at("source").storage);
      source["revision"] = gemm_json::Value(std::string(40, 'a'));
      source["tree_state"] = gemm_json::Value("clean");
      auto& build = std::get<gemm_json::Object>(payload.at("build").storage);
      build["compiler_id"] = gemm_json::Value("fixture-compiler");
      build["compiler_version"] = gemm_json::Value("1.0");
      build["cxx_standard"] = gemm_json::Value("20");
      build["build_type"] = gemm_json::Value("Release");
      build["platform"] = gemm_json::Value("fixture-platform");
      build["cmake_version"] = gemm_json::Value("3.30");
      auto& executable_record = std::get<gemm_json::Object>(payload.at("executable").storage);
      executable_record["status"] = gemm_json::Value("hashed");
      executable_record["sha256"] = gemm_json::Value(std::string(64, 'b'));
      auto& manifest_record = std::get<gemm_json::Object>(payload.at("manifest").storage);
      auto& manifest_identity = std::get<gemm_json::Object>(manifest_record.at("identity").storage);
      manifest_identity["status"] = gemm_json::Value("hashed");
      auto& files = std::get<gemm_json::Array>(payload.at("configuration_files").storage);
      for (auto& file : files)
        std::get<gemm_json::Object>(file.storage)["status"] = gemm_json::Value("hashed");
      auto& input_verification = std::get<gemm_json::Object>(payload.at("input_verification").storage);
      input_verification["status"] = gemm_json::Value("exact_bytes_captured_and_used");
      auto& package = std::get<gemm_json::Object>(payload.at("package").storage);
      package["kind"] = gemm_json::Value("validation");
      package["id"] = gemm_json::Value("validation-fixture");
      package["release_tag"] = gemm_json::Value("unknown");
      payload["result"] = gemm_json::Value(gemm_json::Object{
          {"sha256", gemm_json::Value(std::string(64, 'c'))}, {"status", gemm_json::Value("hashed")}});
      return record;
    };
    const auto complete_trace_fixture = make_complete_fixture(arrival_record);
    require(!run_provenance_completeness_issue(complete_trace_fixture),
            "a complete trace-run provenance fixture with resolved runtime inputs must pass structural validation");
    auto fractional_frame_fixture = complete_trace_fixture;
    auto& fractional_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(fractional_frame_fixture.storage).at("provenance").storage);
    auto& fractional_config = std::get<gemm_json::Object>(fractional_payload.at("configuration").storage);
    fractional_config["frames"] = gemm_json::Value(gemm_json::Number{"1.5"});
    const auto fractional_issue = run_provenance_completeness_issue(fractional_frame_fixture);
    require(fractional_issue && fractional_issue->find("signed integer") != std::string::npos,
            "a complete claim with a fractional integer configuration value must be rejected");
    auto nonfinite_fixture = complete_trace_fixture;
    auto& nonfinite_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(nonfinite_fixture.storage).at("provenance").storage);
    auto& nonfinite_config = std::get<gemm_json::Object>(nonfinite_payload.at("configuration").storage);
    nonfinite_config["frame_period_us"] = gemm_json::Value(gemm_json::Number{"1e999"});
    const auto nonfinite_issue = run_provenance_completeness_issue(nonfinite_fixture);
    require(nonfinite_issue && nonfinite_issue->find("non-finite") != std::string::npos,
            "a complete claim with an out-of-range numeric configuration value must be rejected");
    auto cxx_standard_fixture = complete_trace_fixture;
    auto& cxx_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(cxx_standard_fixture.storage).at("provenance").storage);
    auto& cxx_build = std::get<gemm_json::Object>(cxx_payload.at("build").storage);
    cxx_build["cxx_standard"] = gemm_json::Value("17");
    const auto cxx_issue = run_provenance_completeness_issue(cxx_standard_fixture);
    require(cxx_issue && cxx_issue->find("C++ standard") != std::string::npos,
            "a complete claim with an unsupported C++ standard must be rejected consistently");
    auto missing_override_fixture = complete_trace_fixture;
    auto& missing_override_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(missing_override_fixture.storage).at("provenance").storage);
    missing_override_payload.erase("explicit_cli_overrides");
    const auto missing_override_issue = run_provenance_completeness_issue(missing_override_fixture);
    require(missing_override_issue && missing_override_issue->find("overrides") != std::string::npos,
            "a complete claim must include the explicit override and authored-value maps");
    auto wrong_trace_fixture = complete_trace_fixture;
    auto& wrong_trace_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(wrong_trace_fixture.storage).at("provenance").storage);
    auto& wrong_trace_files = std::get<gemm_json::Array>(wrong_trace_payload.at("configuration_files").storage);
    const auto trace_file = std::find_if(wrong_trace_files.begin(), wrong_trace_files.end(), [](const auto& item) {
      return item.object().at("role").string() == "frame_arrival_trace";
    });
    require(trace_file != wrong_trace_files.end(), "trace fixture must contain its declared trace input");
    std::get<gemm_json::Object>(trace_file->storage)["path"] = gemm_json::Value("substituted.csv");
    const auto wrong_trace_issue = run_provenance_completeness_issue(wrong_trace_fixture);
    require(wrong_trace_issue && wrong_trace_issue->find("frame-arrival inputs") != std::string::npos,
            "a complete claim must reject a trace path that differs from the resolved configuration");
    auto missing_trace_fixture = complete_trace_fixture;
    auto& missing_trace_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(missing_trace_fixture.storage).at("provenance").storage);
    auto& missing_trace_files = std::get<gemm_json::Array>(missing_trace_payload.at("configuration_files").storage);
    missing_trace_files.erase(std::remove_if(missing_trace_files.begin(), missing_trace_files.end(), [](const auto& item) {
      return item.object().at("role").string() == "frame_arrival_trace";
    }), missing_trace_files.end());
    const auto missing_trace_issue = run_provenance_completeness_issue(missing_trace_fixture);
    require(missing_trace_issue && missing_trace_issue->find("frame-arrival inputs") != std::string::npos,
            "a complete claim must reject a configured trace with no hashed input record");
    write(arrival_trace, "frame,release_us\n0,9\n");
    validate(arrival_loaded.config,
             *captured_runtime_input(arrival_captured, "frame_arrival_trace"),
             *captured_runtime_input(arrival_captured, "frame_arrival_provenance"));
    Simulator captured_simulator(arrival_loaded.config);
    captured_simulator.run(arrival_prepared);
    require(!captured_simulator.trace().empty() && captured_simulator.trace().front().start_us == 0.0,
            "simulation must consume captured arrival values even when the file changes after capture");
    write(arrival_trace, "frame,release_us\n0,0\n");
    const auto changed_inputs = finalize_run_provenance(arrival_record, result_file);
    require(field(changed_inputs, "identity_status").string() == field(arrival_record, "identity_status").string(),
            "filesystem mutations after capture must not invalidate or rewrite the identity of bytes actually consumed");
    const auto changed_identity_status = field(finalized, "identity_status").string() == "complete"
        ? "incomplete" : "complete";
    const auto changed_status_input = gemm_json::Value(gemm_json::Object{
        {"identity_status", gemm_json::Value(changed_identity_status)},
        {"provenance", field(finalized, "provenance")}});
    require(sha256_hex(canonical_provenance_value(changed_status_input)) !=
                field(finalized, "identity_sha256").string(),
            "identity status must participate in the canonical integrity hash");
    write(arrival_trace, "frame,release_us\n0,1\n");
    const auto trace_item = std::find_if(arrival_files.begin(), arrival_files.end(), [](const auto& input) {
      return input.object().at("role").string() == "frame_arrival_trace";
    });
    require(trace_item != arrival_files.end() &&
                trace_item->object().at("sha256").string() == sha256_hex("frame,release_us\n0,0\n"),
            "a provenance record must retain the hash of the exact input bytes captured for execution");
    const auto changed_config = load_config_with_sources(arrival_config.string());
    const auto changed_capture = capture_runtime_inputs(changed_config.config);
    require(canonical_provenance_value(arrival_record) != canonical_provenance_value(
                make_run_provenance(changed_config, arrival_config, executable, changed_capture)),
            "a changed referenced runtime input must produce a new identity for a later run");

    const auto profile_path = root / "configs" / "tiny-profile.json";
    const auto mapping_path = root / "configs" / "tiny-mapping.json";
    const auto evidence_path = root / "configs" / "tiny-evidence.json";
    const auto graph_manifest = root / "configs" / "graph.conf";
    const std::string model_hash(64, '0');
    const std::string profile_json =
        "{\"schema_version\":\"axissim.workload-profile.v1\","
        "\"source_url\":\"https://example.invalid/model\",\"license\":\"CC0-1.0\","
        "\"model_sha256\":\"" + model_hash + "\",\"tensor_storage_precision\":\"fp32\","
        "\"operators\":[{\"id\":\"op\",\"mapping_class\":\"npu_compute\","
        "\"dependencies\":[],\"inputs\":[\"input\"],\"outputs\":[\"output\"],"
        "\"macs\":1,\"vector_elements\":0,\"ops_per_element\":0,"
        "\"activation_read_bytes\":1,\"activation_write_bytes\":1,\"weight_bytes\":1}]}";
    const std::string evidence_json =
        "{\"schema_version\":\"axissim.accuracy-evidence.v1\","
        "\"mapping_plan_id\":\"captured_plan\",\"source_model_sha256\":\"" + model_hash + "\","
        "\"evaluation_status\":\"external_evaluated\",\"metric\":\"accuracy\","
        "\"value\":1.0,\"dataset\":\"fixture\",\"evaluator\":\"fixture\","
        "\"source_url\":\"https://example.invalid/evidence\"}";
    const std::string mapping_json =
        "{\"schema_version\":\"axissim.mapping-plan.v1\",\"id\":\"captured_plan\","
        "\"source_model_sha256\":\"" + model_hash + "\",\"mapping_rule_version\":\"test-v1\","
        "\"accuracy_evidence\":\"tiny-evidence.json\",\"entries\":["
        "{\"operator_id\":\"op\",\"mapping_class\":\"npu_compute\"}]}";
    write(profile_path, profile_json);
    write(mapping_path, mapping_json);
    write(evidence_path, evidence_json);
    write(graph_manifest,
          "scenario_name = captured_graph_fixture\n"
          "workload_mode = profiled_graph\n"
          "workload_profile = tiny-profile.json\n"
          "mapping_plan = tiny-mapping.json\n"
          "frames = 1\n");
    const auto graph_loaded = load_config_with_sources(graph_manifest.string());
    const auto graph_capture = capture_runtime_inputs(graph_loaded.config);
    validate(graph_loaded.config);
    write(profile_path, "invalid replacement profile\n");
    write(mapping_path, "invalid replacement mapping\n");
    write(evidence_path, "invalid replacement evidence\n");
    const auto graph_prepared = prepare_run_inputs(graph_loaded.config, graph_capture);
    require(graph_prepared.workload_profile &&
                graph_prepared.workload_profile->operators.front().macs == 1 &&
                graph_prepared.workload_profile->mapping_plan_id == "captured_plan" &&
                graph_prepared.workload_profile->accuracy_evidence_status == "external_evaluated",
            "profile, mapping plan, and nested accuracy evidence must all parse from the captured bytes");
    Simulator graph_simulator(graph_loaded.config);
    graph_simulator.run(graph_prepared);
    require(std::any_of(graph_simulator.trace().begin(), graph_simulator.trace().end(), [](const auto& event) {
              return event.task == "profile_npu:op";
            }),
            "the simulator must execute the parsed captured profile after its files change");
    const auto graph_record = make_run_provenance(graph_loaded, graph_manifest, executable, graph_capture);
    const auto complete_graph_fixture = make_complete_fixture(graph_record);
    require(!run_provenance_completeness_issue(complete_graph_fixture),
            "a complete profiled-graph record with its captured inputs must pass structural validation");
    auto changed_profile_path = complete_graph_fixture;
    auto& changed_profile_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(changed_profile_path.storage).at("provenance").storage);
    auto& changed_profile_config = std::get<gemm_json::Object>(changed_profile_payload.at("configuration").storage);
    changed_profile_config["workload_profile"] = gemm_json::Value("substituted-profile.json");
    const auto changed_profile_issue = run_provenance_completeness_issue(changed_profile_path);
    require(changed_profile_issue && changed_profile_issue->find("profiled-workload inputs") != std::string::npos,
            "a complete claim must reject a profile path that differs from the resolved configuration");
    auto omitted_evidence_record = graph_record;
    auto& omitted_evidence_payload = std::get<gemm_json::Object>(
        std::get<gemm_json::Object>(omitted_evidence_record.storage).at("provenance").storage);
    auto& omitted_evidence_files = std::get<gemm_json::Array>(
        omitted_evidence_payload.at("configuration_files").storage);
    omitted_evidence_files.erase(std::remove_if(omitted_evidence_files.begin(), omitted_evidence_files.end(), [](const auto& item) {
      return item.object().at("role").string() == "mapping_accuracy_evidence";
    }), omitted_evidence_files.end());
    const auto omitted_evidence_issue = run_provenance_completeness_issue(make_complete_fixture(omitted_evidence_record));
    require(omitted_evidence_issue && omitted_evidence_issue->find("runtime-input") != std::string::npos,
            "a complete claim must reject an omitted nested mapping-evidence record");
    std::vector<CapturedRuntimeInput> missing_evidence_capture;
    std::copy_if(graph_capture.begin(), graph_capture.end(), std::back_inserter(missing_evidence_capture), [](const auto& input) {
      return input.role != "mapping_accuracy_evidence";
    });
    const auto producer_omitted_evidence = make_run_provenance(
        graph_loaded, graph_manifest, executable, missing_evidence_capture);
    require(field(producer_omitted_evidence, "identity_status").string() == "incomplete",
            "the simulator must not create a complete identity when nested mapping evidence is not captured");
    const auto& graph_files = field(field(graph_record, "provenance"), "configuration_files").array();
    const auto evidence_item = std::find_if(graph_files.begin(), graph_files.end(), [](const auto& item) {
      return item.object().at("role").string() == "mapping_accuracy_evidence";
    });
    require(evidence_item != graph_files.end() &&
                evidence_item->object().at("sha256").string() == sha256_hex(evidence_json),
            "nested mapping accuracy evidence provenance must hash the exact bytes parsed");

    const auto overridden = load_config_with_sources(manifest.string(), {{"npu_macs", "512"}});
    const auto override_inputs = capture_runtime_inputs(overridden.config);
    const auto override_record = make_run_provenance(overridden, manifest, executable, override_inputs);
    require(canonical_provenance_value(override_record) != first_canonical,
            "explicit CLI overrides must affect provenance identity");
    require(field(field(override_record, "provenance"), "explicit_cli_overrides").object().at("npu_macs").string() == "512",
            "the explicit override must be recorded separately");

    write(included, "frames = 1\nnpu_macs = 2048\n");
    const auto modified_include = load_config_with_sources(manifest.string());
    const auto include_inputs = capture_runtime_inputs(modified_include.config);
    const auto include_record = make_run_provenance(modified_include, manifest, executable, include_inputs);
    require(canonical_provenance_value(include_record) != first_canonical,
            "a changed transitive configuration input must change provenance identity");

    const auto missing_executable = make_run_provenance(modified_include, manifest, root / "missing-executable", include_inputs);
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
