#!/usr/bin/env python3
"""Checks the public run-result/provenance sidecar contract end to end."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from provenance_contract import RawNumber, canonical_json, parse_run_provenance


def run(binary: pathlib.Path, config: pathlib.Path, output: pathlib.Path, *overrides: str) -> dict:
    trace = output.with_suffix(".trace.csv")
    command = [str(binary), "run", str(config), "--json", str(output), "--trace", str(trace)]
    for assignment in overrides:
        command.extend(["--set", assignment])
    subprocess.run(command, check=True, capture_output=True, text=True)
    result = json.loads(output.read_text(encoding="utf-8"))
    sidecar_path = pathlib.Path(str(output) + ".provenance.json")
    provenance = parse_run_provenance(
        sidecar_path.read_text(encoding="utf-8"), result_json_bytes=output.read_bytes()
    )
    if not trace.is_file() or trace.stat().st_size == 0:
        raise AssertionError("run did not emit a task trace")
    if result.get("schema_version") != "1.2":
        raise AssertionError("legacy result JSON schema changed")
    if provenance.get("provenance", {}).get("schema_version") != "axissim.run-provenance.v2":
        raise AssertionError("unsupported provenance schema")
    if provenance["provenance"].get("input_verification", {}).get("status") != "exact_bytes_captured_and_used":
        raise AssertionError("run inputs were not tied to the exact bytes parsed and executed")
    return {
        "result": result,
        "provenance": provenance,
        "sidecar_text": sidecar_path.read_text(encoding="utf-8"),
    }


def seal_identity(record: dict) -> str:
    payload = record["provenance"]
    record["identity_sha256"] = hashlib.sha256(canonical_json({
        "identity_status": record["identity_status"], "provenance": payload
    }).encode("utf-8")).hexdigest()
    return canonical_json(record)


def resign_complete(record: dict) -> str:
    record["identity_status"] = "complete"
    payload = record["provenance"]
    payload["source"].update({"revision": "a" * 40, "tree_state": "clean"})
    payload["build"].update({
        "compiler_id": "fixture-compiler", "compiler_version": "1.0", "cxx_standard": "20",
        "build_type": "Release", "platform": "fixture-platform", "cmake_version": "3.30",
    })
    payload["executable"].update({"status": "hashed", "sha256": "b" * 64})
    payload["package"].update({"kind": "validation", "id": "validation-fixture", "release_tag": "unknown"})
    payload["manifest"]["identity"].update({"role": "root_configuration", "status": "hashed"})
    for input_record in payload["configuration_files"]:
        input_record.update({"status": "hashed"})
    payload["input_verification"]["status"] = "exact_bytes_captured_and_used"
    payload["result"]["status"] = "hashed"
    return seal_identity(record)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    args = parser.parse_args()
    source = args.source_root.resolve()
    binary = args.binary.resolve()
    source_revision = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()

    with tempfile.TemporaryDirectory(prefix="axissim-provenance-e2e-") as temporary:
        temp = pathlib.Path(temporary)
        original_config = source / "configs/baseline/manifest.conf"
        first = run(binary, original_config, temp / "baseline-a.json")
        second = run(binary, original_config, temp / "baseline-b.json")
        if first["provenance"] != second["provenance"]:
            raise AssertionError("same executable and inputs did not produce identical sidecars")
        try:
            parse_run_provenance(first["sidecar_text"], require_complete=True)
        except ValueError as error:
            if "result JSON bytes" not in str(error):
                raise
        else:
            raise AssertionError("complete sidecar was accepted without its corresponding result bytes")

        damaged_payload = first["sidecar_text"].replace("baseline_physical_ai", "tampered_scenario", 1)
        if damaged_payload == first["sidecar_text"]:
            raise AssertionError("test fixture did not contain an identifiable scenario field")
        try:
            parse_run_provenance(damaged_payload)
        except ValueError as error:
            if "SHA-256" not in str(error):
                raise
        else:
            raise AssertionError("a changed provenance payload was accepted under the old identity hash")
        original_status = first["provenance"]["identity_status"]
        changed_status = "complete" if original_status == "incomplete" else "incomplete"
        tampered_status = first["sidecar_text"].replace(
            f'"identity_status":"{original_status}"',
            f'"identity_status":"{changed_status}"',
            1,
        )
        if tampered_status == first["sidecar_text"]:
            raise AssertionError("test fixture did not contain an identifiable identity status")
        try:
            parse_run_provenance(tampered_status)
        except ValueError:
            pass
        else:
            raise AssertionError("changing identity status was accepted without a valid complete claim or identity hash")

        forged = json.loads(first["sidecar_text"], parse_int=RawNumber, parse_float=RawNumber)
        forged["identity_status"] = "complete"
        forged_payload = forged["provenance"]
        forged_payload["source"].update({"revision": "unknown", "tree_state": "unknown"})
        forged_payload["build"].update({"compiler_id": "unknown", "compiler_version": "unknown"})
        forged_payload["executable"].update({"status": "unavailable", "sha256": "unknown"})
        forged_payload["manifest"]["identity"].update({"status": "unavailable", "sha256": "unknown"})
        for input_record in forged_payload["configuration_files"]:
            input_record.update({"status": "unavailable", "sha256": "unknown"})
        forged["identity_sha256"] = hashlib.sha256(canonical_json({
            "identity_status": "complete", "provenance": forged_payload
        }).encode("utf-8")).hexdigest()
        try:
            parse_run_provenance(
                canonical_json(forged), result_json_bytes=(temp / "baseline-a.json").read_bytes(),
                require_complete=True,
            )
        except ValueError as error:
            if "source revision" not in str(error):
                raise
        else:
            raise AssertionError("a self-hashed complete claim with unavailable source/tool/config identity was accepted")

        valid_complete = json.loads(first["sidecar_text"], parse_int=RawNumber, parse_float=RawNumber)
        complete_text = resign_complete(valid_complete)
        parse_run_provenance(
            complete_text, result_json_bytes=(temp / "baseline-a.json").read_bytes(), require_complete=True
        )
        for mutate, expected in (
            (lambda payload: payload["configuration"].update({"frames": RawNumber("1.5")}), "integer"),
            (lambda payload: payload["configuration"].update({"frame_period_us": RawNumber("1e999")}), "non-finite"),
            (lambda payload: payload["build"].update({"cxx_standard": "17"}), "C++ standard"),
            (lambda payload: payload.pop("explicit_cli_overrides"), "overrides"),
            (lambda payload: payload.pop("authored_values_after_overrides"), "authored configuration"),
        ):
            invalid_record = json.loads(complete_text, parse_int=RawNumber, parse_float=RawNumber)
            mutate(invalid_record["provenance"])
            invalid_text = seal_identity(invalid_record)
            try:
                parse_run_provenance(
                    invalid_text, result_json_bytes=(temp / "baseline-a.json").read_bytes(), require_complete=True
                )
            except ValueError as error:
                if expected not in str(error):
                    raise
            else:
                raise AssertionError(f"complete provenance with invalid {expected} was accepted")

        trace_dir = temp / "trace-inputs"
        trace_dir.mkdir()
        trace_config = trace_dir / "manifest.conf"
        trace_config.write_text(
            "scenario_name = provenance_trace_fixture\n"
            "frame_arrival_mode = trace\n"
            "frame_arrival_trace = arrivals.csv\n"
            "frame_arrival_provenance = arrivals.md\n"
            "frames = 1\n",
            encoding="utf-8",
        )
        (trace_dir / "arrivals.csv").write_text("frame,release_us\n0,0\n", encoding="utf-8")
        (trace_dir / "arrivals.md").write_text("synthetic trace provenance fixture\n", encoding="utf-8")
        trace_result = run(binary, trace_config, temp / "trace-input-result.json")
        trace_complete = json.loads(trace_result["sidecar_text"], parse_int=RawNumber, parse_float=RawNumber)
        trace_complete_text = resign_complete(trace_complete)
        parse_run_provenance(
            trace_complete_text,
            result_json_bytes=(temp / "trace-input-result.json").read_bytes(),
            require_complete=True,
        )
        for role, mutate_path in (
            ("frame_arrival_trace", lambda item: item.update({"path": "substituted.csv"})),
            ("frame_arrival_provenance", lambda item: item.update({"path": "substituted.md"})),
        ):
            invalid_record = json.loads(trace_complete_text, parse_int=RawNumber, parse_float=RawNumber)
            item = next(item for item in invalid_record["provenance"]["configuration_files"] if item["role"] == role)
            mutate_path(item)
            try:
                parse_run_provenance(
                    seal_identity(invalid_record),
                    result_json_bytes=(temp / "trace-input-result.json").read_bytes(),
                    require_complete=True,
                )
            except ValueError as error:
                if "frame-arrival inputs" not in str(error):
                    raise
            else:
                raise AssertionError(f"complete provenance accepted substituted {role} path")
        invalid_record = json.loads(trace_complete_text, parse_int=RawNumber, parse_float=RawNumber)
        invalid_record["provenance"]["configuration_files"] = [
            item for item in invalid_record["provenance"]["configuration_files"]
            if item["role"] != "frame_arrival_trace"
        ]
        try:
            parse_run_provenance(
                seal_identity(invalid_record),
                result_json_bytes=(temp / "trace-input-result.json").read_bytes(),
                require_complete=True,
            )
        except ValueError as error:
            if "frame-arrival inputs" not in str(error):
                raise
        else:
            raise AssertionError("complete provenance accepted a trace configuration without its input hash")

        graph_dir = temp / "profiled-graph-inputs"
        graph_dir.mkdir()
        graph_config = graph_dir / "manifest.conf"
        model_hash = "0" * 64
        (graph_dir / "profile.json").write_text(
            "{\"schema_version\":\"axissim.workload-profile.v1\","
            "\"source_url\":\"https://example.invalid/model\",\"license\":\"CC0-1.0\","
            f"\"model_sha256\":\"{model_hash}\",\"tensor_storage_precision\":\"fp32\","
            "\"operators\":[{\"id\":\"op\",\"mapping_class\":\"npu_compute\","
            "\"dependencies\":[],\"inputs\":[\"input\"],\"outputs\":[\"output\"],"
            "\"macs\":1,\"vector_elements\":0,\"ops_per_element\":0,"
            "\"activation_read_bytes\":1,\"activation_write_bytes\":1,\"weight_bytes\":1}]}",
            encoding="utf-8",
        )
        (graph_dir / "mapping.json").write_text(
            "{\"schema_version\":\"axissim.mapping-plan.v1\",\"id\":\"captured_plan\","
            f"\"source_model_sha256\":\"{model_hash}\",\"mapping_rule_version\":\"test-v1\","
            "\"accuracy_evidence\":\"evidence.json\",\"entries\":["
            "{\"operator_id\":\"op\",\"mapping_class\":\"npu_compute\"}]}",
            encoding="utf-8",
        )
        (graph_dir / "evidence.json").write_text(
            "{\"schema_version\":\"axissim.accuracy-evidence.v1\","
            "\"mapping_plan_id\":\"captured_plan\","
            f"\"source_model_sha256\":\"{model_hash}\","
            "\"evaluation_status\":\"external_evaluated\",\"metric\":\"accuracy\","
            "\"value\":1.0,\"dataset\":\"fixture\",\"evaluator\":\"fixture\","
            "\"source_url\":\"https://example.invalid/evidence\"}",
            encoding="utf-8",
        )
        graph_config.write_text(
            "scenario_name = provenance_profiled_graph_fixture\n"
            "workload_mode = profiled_graph\n"
            "workload_profile = profile.json\n"
            "mapping_plan = mapping.json\n"
            "frames = 1\n",
            encoding="utf-8",
        )
        graph_result_path = temp / "profiled-graph-result.json"
        graph_result = run(binary, graph_config, graph_result_path)
        graph_complete = json.loads(graph_result["sidecar_text"], parse_int=RawNumber, parse_float=RawNumber)
        graph_complete_text = resign_complete(graph_complete)
        parse_run_provenance(
            graph_complete_text,
            result_json_bytes=graph_result_path.read_bytes(),
            require_complete=True,
        )
        graph_roles = {
            item["role"] for item in graph_complete["provenance"]["configuration_files"]
        }
        if not {"workload_profile", "mapping_plan", "mapping_accuracy_evidence"}.issubset(graph_roles):
            raise AssertionError("profiled workload provenance omitted a captured mapping dependency")
        graph_mutations = (
            (
                lambda payload: payload["configuration"].update({"workload_profile": "substituted-profile.json"}),
                "profiled-workload inputs",
            ),
            (
                lambda payload: payload["configuration_files"].remove(next(
                    item for item in payload["configuration_files"] if item["role"] == "mapping_accuracy_evidence"
                )),
                "runtime-input requirements",
            ),
        )
        for mutate, expected in graph_mutations:
            invalid_record = json.loads(graph_complete_text, parse_int=RawNumber, parse_float=RawNumber)
            mutate(invalid_record["provenance"])
            try:
                parse_run_provenance(
                    seal_identity(invalid_record),
                    result_json_bytes=graph_result_path.read_bytes(),
                    require_complete=True,
                )
            except ValueError as error:
                if expected not in str(error):
                    raise
            else:
                raise AssertionError(f"complete profiled-graph provenance accepted {expected} corruption")

        tampered_result = first["result"].copy()
        tampered_result["scenario_name"] = "tampered_result"
        try:
            parse_run_provenance(
                first["sidecar_text"],
                result_json_bytes=(json.dumps(tampered_result, separators=(",", ":")) + "\n").encode(),
            )
        except ValueError as error:
            if "result JSON" not in str(error):
                raise
        else:
            raise AssertionError("a result JSON change was not detected by its provenance sidecar")

        identity = first["provenance"]["provenance"]
        if identity["source"]["revision"] != source_revision:
            raise AssertionError("run provenance does not report the source revision embedded at build time")
        executable_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
        if identity["executable"]["sha256"] != executable_sha:
            raise AssertionError("provenance executable SHA-256 does not match the invoked binary")
        if not re.fullmatch(r"[0-9a-f]{64}", first["provenance"].get("identity_sha256", "")):
            raise AssertionError("canonical provenance identity hash is missing or malformed")
        if identity["source"]["revision"] in ("", "unknown"):
            if first["provenance"]["identity_status"] != "incomplete":
                raise AssertionError("unknown source identity must be marked incomplete")
        if pathlib.Path(identity["manifest"]["path"]).is_absolute():
            raise AssertionError("provenance contains an unstable absolute manifest path")
        if str(temp) in json.dumps(first["provenance"], sort_keys=True):
            raise AssertionError("canonical provenance contains a machine-local temporary path")

        changed = run(binary, original_config, temp / "override.json", "npu_macs=512")
        if changed["provenance"]["identity_sha256"] == first["provenance"]["identity_sha256"]:
            raise AssertionError("a changed explicit override did not change the identity")
        overrides = changed["provenance"]["provenance"]["explicit_cli_overrides"]
        if overrides != {"npu_macs": "512"}:
            raise AssertionError("the CLI override was not recorded exactly")

        config_dir = temp / "relocated-inputs"
        config_dir.mkdir()
        root_manifest = config_dir / "manifest.conf"
        layer = config_dir / "architecture.conf"
        root_manifest.write_text("include = architecture.conf\nscenario_name = provenance_e2e\n", encoding="utf-8")
        layer.write_text("frames = 1\nnpu_macs = 1024\n", encoding="utf-8")
        before = run(binary, root_manifest, temp / "include-before.json")
        layer.write_text("frames = 1\nnpu_macs = 2048\n", encoding="utf-8")
        after = run(binary, root_manifest, temp / "include-after.json")
        before_files = before["provenance"]["provenance"]["configuration_files"]
        after_files = after["provenance"]["provenance"]["configuration_files"]
        before_include = next(item for item in before_files if item["path"] == "architecture.conf")
        after_include = next(item for item in after_files if item["path"] == "architecture.conf")
        if before_include["sha256"] == after_include["sha256"]:
            raise AssertionError("a changed included configuration file was not detected")
        if before["provenance"]["identity_sha256"] == after["provenance"]["identity_sha256"]:
            raise AssertionError("a changed transitive config did not change canonical identity")
        for record in before_files:
            if pathlib.Path(record["path"]).is_absolute():
                raise AssertionError("transitive configuration provenance leaked an absolute path")

        baselines = json.loads((source / "tests/reference/legacy_study_baselines.json").read_text(encoding="utf-8"))
        baseline = baselines["cli_baselines"]["physical_ai_baseline"]["result_json"]
        got = first["result"]
        expected = baseline
        comparisons = [
            (got["schema_version"], "1.2", "result schema"),
            (got["scenario_name"], expected["scenario_name"], "scenario"),
            (got["model_version"], expected["model_version"], "model version"),
            (got["latency"]["mean_us"], expected["latency"]["mean_us"], "mean latency"),
            (got["traffic"]["dram_bytes"], expected["traffic"]["dram_bytes"], "DRAM bytes"),
            (got["energy"]["per_frame_uj"], expected["energy"]["per_frame_uj"], "dynamic energy/frame"),
        ]
        for observed, wanted, label in comparisons:
            if observed != wanted:
                raise AssertionError(f"legacy numerical output changed for {label}: {observed!r} != {wanted!r}")

    if hashlib.sha256(b"abc").hexdigest() != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad":
        raise AssertionError("independent SHA-256 fixture failed")
    print("run provenance sidecar E2E passed: result binding, status integrity, reproducibility, executable/config identity, override, transitive include, and schema 1.2 parity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
