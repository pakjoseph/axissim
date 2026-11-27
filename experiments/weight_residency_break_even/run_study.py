#!/usr/bin/env python3
"""Run and summarize the bounded MNIST-12 weight-residency comparison."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from decimal import Decimal
from typing import Any


HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_MANIFEST = HERE / "manifest.json"
DEFAULT_TIMEOUT_SECONDS = 45


class StudyError(RuntimeError):
    pass


def sha256_bytes(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: pathlib.Path) -> Any:
    with path.open("r", encoding="utf-8") as source:
        return json.load(source, parse_float=Decimal)


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False, default=_json_default) + "\n").encode("utf-8")


def _json_default(value: Any) -> Any:
    if isinstance(value, Decimal):
        return format(value, "f")
    raise TypeError(f"unsupported JSON value: {type(value).__name__}")


def decimal_text(value: Decimal) -> str:
    return format(value, "f")


def metric(result: dict[str, Any], *keys: str) -> Decimal:
    value: Any = result
    for key in keys:
        value = value[key]
    if isinstance(value, bool) or not isinstance(value, (int, Decimal)):
        raise StudyError(f"result field {'.'.join(keys)} is not numeric")
    return Decimal(value)


def load_manifest(path: pathlib.Path) -> tuple[dict[str, Any], bytes]:
    raw = path.read_bytes()
    try:
        manifest = json.loads(raw, parse_float=Decimal)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise StudyError(f"invalid study manifest: {error}") from error
    if manifest.get("schema_id") != "axissim.weight-residency-break-even.v1":
        raise StudyError("unsupported M8.0 study manifest schema")
    workload = manifest.get("workload", {})
    architecture = manifest.get("architecture_controls", {})
    sensitivity = manifest.get("energy_sensitivity", {})
    if workload.get("measurement_protocol") != "finite_completed_frames" or workload.get("frames") != 30:
        raise StudyError("M8.0 requires the approved 30-frame finite measurement protocol")
    if architecture.get("expected_persistent_weight_bytes") != 13600:
        raise StudyError("M8.0 manifest no longer names the reviewed 13,600-byte weight set")
    if architecture.get("weight_sram_capacity_bytes") != 16384 or architecture.get("capacity_negative_control_bytes") != 8192:
        raise StudyError("M8.0 capacity controls differ from the preregistered values")
    multipliers = sensitivity.get("one_at_a_time_multipliers")
    if multipliers != ["0.5", "1", "2"] or not sensitivity.get("include_base_pair"):
        raise StudyError("M8.0 energy sensitivity must retain the preregistered factor-of-two points")
    if sensitivity.get("additional_repeats_per_base_arm") != 1:
        raise StudyError("M8.0 requires exactly one additional base-pair replay per arm")
    expected_static = {"static_power_w", "npu_static_power_per_mac_w", "rvv_static_power_per_lane_w",
                       "sram_static_power_per_kib_w", "dram_interface_static_power_w"}
    static_overrides = manifest.get("static_power_overrides_w", {})
    if set(static_overrides) != expected_static or any(Decimal(value) != 0 for value in static_overrides.values()):
        raise StudyError("M8.0 static-power overrides must explicitly set the five supported terms to zero")
    return manifest, raw


def _manifest_path(root: pathlib.Path, value: str) -> pathlib.Path:
    path = (root / value).resolve()
    if not path.is_file():
        raise StudyError(f"required study input is missing: {value}")
    try:
        path.relative_to(root.resolve())
    except ValueError as error:
        raise StudyError(f"study input escapes the repository: {value}") from error
    return path


def _mapping_override(root: pathlib.Path, manifest_value: str) -> str:
    path = _manifest_path(root, manifest_value)
    # Config file paths are resolved relative to configs/workloads/ by C++.
    return os.path.relpath(path, root / "configs" / "workloads")


def _read_result_artifacts(output_dir: pathlib.Path, run_id: str) -> dict[str, Any]:
    result_path = output_dir / "runs" / f"{run_id}.json"
    provenance_path = output_dir / "runs" / f"{run_id}.json.provenance.json"
    trace_path = output_dir / "runs" / f"{run_id}.trace.csv"
    result = load_json(result_path)
    provenance_envelope = load_json(provenance_path)
    provenance = provenance_envelope.get("provenance", {})
    if provenance_envelope.get("identity_status") != "complete":
        raise StudyError(f"{run_id} has incomplete run provenance")
    if provenance.get("schema_version") != "axissim.run-provenance.v2":
        raise StudyError(f"{run_id} has an unsupported provenance schema")
    if provenance.get("result", {}).get("sha256") != sha256_file(result_path):
        raise StudyError(f"{run_id} provenance does not bind its result JSON")
    if not trace_path.is_file():
        raise StudyError(f"{run_id} trace CSV is missing")
    if provenance.get("source", {}).get("tree_state") != "clean":
        raise StudyError(f"{run_id} source tree was not clean at build time")
    if not provenance.get("source", {}).get("revision") or not provenance.get("executable", {}).get("sha256"):
        raise StudyError(f"{run_id} is missing source or executable identity")
    input_roles = {entry.get("role") for entry in provenance.get("configuration_files", [])
                   if entry.get("status") == "hashed"}
    if not {"root_configuration", "workload_profile", "mapping_plan"}.issubset(input_roles):
        raise StudyError(f"{run_id} provenance does not hash the root config, workload profile, and mapping plan")
    return {
        "run_id": run_id,
        "result_path": result_path,
        "provenance_path": provenance_path,
        "trace_path": trace_path,
        "result": result,
        "provenance_envelope": provenance_envelope,
        "provenance": provenance,
        "result_sha256": sha256_file(result_path),
        "provenance_sha256": sha256_file(provenance_path),
        "trace_sha256": sha256_file(trace_path),
    }


def _invoke(binary: pathlib.Path, config: pathlib.Path, mapping: str, capacity: int,
            dram_pj_per_byte: Decimal, dma_pj_per_byte: Decimal, run_id: str,
            output_dir: pathlib.Path, timeout_seconds: int,
            static_power_overrides: dict[str, str], expected_revision: str,
            expected_executable_sha256: str) -> dict[str, Any]:
    run_dir = output_dir / "runs"
    run_dir.mkdir(parents=True, exist_ok=True)
    result_path = run_dir / f"{run_id}.json"
    trace_path = run_dir / f"{run_id}.trace.csv"
    command = [
        str(binary), "run", str(config),
        "--set", f"mapping_plan={mapping}",
        "--set", f"weight_sram_capacity_bytes={capacity}",
        "--set", f"dram_byte_pj={decimal_text(dram_pj_per_byte)}",
        "--set", f"dma_byte_pj={decimal_text(dma_pj_per_byte)}",
    ]
    for key, value in sorted(static_power_overrides.items()):
        command.extend(["--set", f"{key}={value}"])
    command.extend(["--json", str(result_path), "--trace", str(trace_path)])
    try:
        process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                 timeout=timeout_seconds, check=False)
    except subprocess.TimeoutExpired as error:
        return {"run_id": run_id, "status": "timed_out", "timeout_seconds": timeout_seconds,
                "message": str(error), "process_return_code": None}
    if process.returncode != 0:
        return {
            "run_id": run_id,
            "status": "failed",
            "process_return_code": process.returncode,
            "stdout": process.stdout,
            "stderr": process.stderr,
        }
    try:
        artifacts = _read_result_artifacts(output_dir, run_id)
        if artifacts["provenance"].get("source", {}).get("revision") != expected_revision:
            raise StudyError(f"{run_id} provenance source revision differs from the current checkout")
        if artifacts["provenance"].get("executable", {}).get("sha256") != expected_executable_sha256:
            raise StudyError(f"{run_id} provenance executable identity differs from the invoked binary")
    except (OSError, KeyError, TypeError, ValueError, StudyError) as error:
        return {"run_id": run_id, "status": "internal_error", "message": str(error),
                "process_return_code": process.returncode}
    artifacts.update({"status": "completed", "process_return_code": process.returncode})
    return artifacts


def _resolved_config(run: dict[str, Any]) -> dict[str, Any]:
    configuration = run["provenance"].get("configuration")
    if not isinstance(configuration, dict):
        raise StudyError(f"{run['run_id']} provenance has no resolved configuration")
    return configuration


def _assert_matched_configuration(no_residency: dict[str, Any], resident: dict[str, Any]) -> None:
    left = dict(_resolved_config(no_residency))
    right = dict(_resolved_config(resident))
    if left.pop("mapping_plan", None) is None or right.pop("mapping_plan", None) is None:
        raise StudyError("paired run provenance omits mapping-plan identity")
    if left != right:
        raise StudyError("paired runs differ in resolved configuration outside the intended mapping-plan change")
    def non_mapping_inputs(run: dict[str, Any]) -> set[tuple[str, str, str]]:
        return {(entry.get("role", ""), entry.get("path", ""), entry.get("sha256", ""))
                for entry in run["provenance"].get("configuration_files", [])
                if entry.get("role") != "mapping_plan"}
    if non_mapping_inputs(no_residency) != non_mapping_inputs(resident):
        raise StudyError("paired runs did not use identical non-mapping configuration and workload inputs")
    if no_residency["provenance"].get("source", {}).get("revision") != resident["provenance"].get("source", {}).get("revision"):
        raise StudyError("paired runs used different AxisSim source revisions")
    if no_residency["provenance"].get("executable", {}).get("sha256") != resident["provenance"].get("executable", {}).get("sha256"):
        raise StudyError("paired runs used different executable identities")


def _run_summary(run: dict[str, Any]) -> dict[str, Any]:
    result = run["result"]
    return {
        "run_id": run["run_id"],
        "result_json": run["result_path"].relative_to(run["result_path"].parents[1]).as_posix(),
        "result_sha256": run["result_sha256"],
        "provenance_json": run["provenance_path"].relative_to(run["provenance_path"].parents[1]).as_posix(),
        "provenance_sha256": run["provenance_sha256"],
        "trace_csv": run["trace_path"].relative_to(run["trace_path"].parents[1]).as_posix(),
        "trace_sha256": run["trace_sha256"],
        "source_revision": run["provenance"]["source"]["revision"],
        "executable_sha256": run["provenance"]["executable"]["sha256"],
        "provenance_identity_sha256": run["provenance_envelope"]["identity_sha256"],
        "scenario_name": result["scenario_name"],
        "mapping_plan": result["mapping_plan"],
        "frames_completed": result["deadlines"]["vision_jobs"],
        "first_frame_logical_bytes": result["profile_transfer_ledger"]["first_frame_bytes"],
        "steady_frame_logical_bytes": result["profile_transfer_ledger"]["steady_state_bytes"],
        "total_logical_dram_bytes": result["traffic"]["dram_bytes"],
        "persistent_weight_bytes_avoided_per_steady_frame": result["mapping_effects"]["persistent_weight_bytes_avoided_per_steady_frame"],
        "persistent_weight_operator_count": result["mapping_effects"]["persistent_weight_operator_count"],
        "dynamic_energy_components_pj": {
            "vector": metric(result, "energy", "vector_pj"),
            "npu_arithmetic": metric(result, "energy", "npu_arithmetic_pj"),
            "dram": metric(result, "energy", "dram_pj"),
            "dma": metric(result, "energy", "dma_pj"),
        },
        "dynamic_energy_per_completed_frame_uj": metric(result, "energy", "per_frame_uj"),
        "static_energy_per_completed_frame_uj": metric(result, "energy", "static_per_frame_uj"),
        "it_energy_per_completed_frame_uj": metric(result, "energy", "it_per_frame_uj"),
        "modeled_static_power_w": metric(result, "power", "modeled_static_w"),
        "makespan_us": metric(result, "timing", "makespan_us"),
        "mean_latency_us": metric(result, "latency", "mean_us"),
        "p99_latency_us": metric(result, "latency", "p99_us"),
        "deadline_misses": result["deadlines"]["vision_misses"],
        "deadline_jobs": result["deadlines"]["vision_jobs"],
    }


def _pair_statistics(no_residency: dict[str, Any], resident: dict[str, Any],
                     dram_pj_per_byte: Decimal, dma_pj_per_byte: Decimal,
                     frame_count: int) -> dict[str, Any]:
    _assert_matched_configuration(no_residency, resident)
    left = _run_summary(no_residency)
    right = _run_summary(resident)
    if left["mapping_plan"] != "mnist12_baseline.json" or right["mapping_plan"] != "mnist12_persistent_weights.json":
        raise StudyError("paired results do not identify the preregistered mapping plans")
    if left["frames_completed"] != frame_count or right["frames_completed"] != frame_count:
        raise StudyError("both arms must complete all preregistered frames")
    if left["first_frame_logical_bytes"] != right["first_frame_logical_bytes"]:
        raise StudyError("first-frame transfer payload changed between mappings")
    if right["persistent_weight_bytes_avoided_per_steady_frame"] <= 0:
        raise StudyError("persistent mapping did not report positive steady-frame weight reuse")
    steady_saving = int(left["steady_frame_logical_bytes"]) - int(right["steady_frame_logical_bytes"])
    if steady_saving != int(right["persistent_weight_bytes_avoided_per_steady_frame"]):
        raise StudyError("steady-state ledger does not reconcile with the reported persistent-weight payload")
    saved_bytes = int(left["total_logical_dram_bytes"]) - int(right["total_logical_dram_bytes"])
    if saved_bytes != steady_saving * (frame_count - 1):
        raise StudyError("total run traffic does not reconcile with first-frame plus steady-state ledgers")
    left_config = _resolved_config(no_residency)
    right_config = _resolved_config(resident)
    if Decimal(str(left_config["dram_byte_pj"])) != dram_pj_per_byte or Decimal(str(right_config["dram_byte_pj"])) != dram_pj_per_byte:
        raise StudyError("resolved DRAM coefficient does not match the declared sensitivity case")
    if Decimal(str(left_config["dma_byte_pj"])) != dma_pj_per_byte or Decimal(str(right_config["dma_byte_pj"])) != dma_pj_per_byte:
        raise StudyError("resolved DMA coefficient does not match the declared sensitivity case")

    logical_dma_payload_saved = saved_bytes  # Same transfer_bytes counter is charged to both model terms.
    dynamic_saved = sum(left["dynamic_energy_components_pj"].values()) - sum(right["dynamic_energy_components_pj"].values())
    dram_saved = left["dynamic_energy_components_pj"]["dram"] - right["dynamic_energy_components_pj"]["dram"]
    dma_saved = left["dynamic_energy_components_pj"]["dma"] - right["dynamic_energy_components_pj"]["dma"]
    static_saved = (left["static_energy_per_completed_frame_uj"] - right["static_energy_per_completed_frame_uj"]) * frame_count * Decimal(1_000_000)
    it_saved = dynamic_saved + static_saved
    return {
        "status": "complete",
        "no_residency": left,
        "persistent_residency": right,
        "logical_dram_bytes_saved": saved_bytes,
        "logical_dma_payload_bytes_saved_same_transfer_count": logical_dma_payload_saved,
        "dram_dynamic_energy_saved_pj": dram_saved,
        "dma_dynamic_energy_saved_pj": dma_saved,
        "total_dynamic_energy_saved_pj": dynamic_saved,
        "modeled_static_energy_saved_pj": static_saved,
        "modeled_it_energy_saved_pj": it_saved,
        "maximum_additional_unpriced_sram_energy_pj_over_observation": it_saved,
        "maximum_additional_unpriced_sram_energy_pj_per_completed_frame": it_saved / frame_count,
        "makespan_reduction_us": left["makespan_us"] - right["makespan_us"],
        "mean_latency_reduction_us": left["mean_latency_us"] - right["mean_latency_us"],
        "p99_latency_reduction_us": left["p99_latency_us"] - right["p99_latency_us"],
        "deadline_misses_avoided": left["deadline_misses"] - right["deadline_misses"],
        "configured_dram_byte_pj": dram_pj_per_byte,
        "configured_dma_byte_pj": dma_pj_per_byte,
    }


def _case_specs(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    energy = manifest["energy_sensitivity"]
    dram = Decimal(energy["base_dram_byte_pj"])
    dma = Decimal(energy["base_dma_byte_pj"])
    low, _, high = (Decimal(value) for value in energy["one_at_a_time_multipliers"])
    return [
        {"case_id": "base", "sensitivity": "primary", "dram": dram, "dma": dma},
        {"case_id": "dram_half", "sensitivity": "DRAM coefficient 0.5x", "dram": dram * low, "dma": dma},
        {"case_id": "dram_double", "sensitivity": "DRAM coefficient 2x", "dram": dram * high, "dma": dma},
        {"case_id": "dma_half", "sensitivity": "DMA coefficient 0.5x", "dram": dram, "dma": dma * low},
        {"case_id": "dma_double", "sensitivity": "DMA coefficient 2x", "dram": dram, "dma": dma * high},
    ]


def _run_manifest(manifest: dict[str, Any], binary: pathlib.Path, output_dir: pathlib.Path,
                  timeout_seconds: int, expected_revision: str,
                  expected_executable_sha256: str) -> dict[str, Any]:
    config = _manifest_path(ROOT, manifest["workload"]["scenario_config"])
    no_map = _mapping_override(ROOT, manifest["architecture_controls"]["no_residency_mapping"])
    resident_map = _mapping_override(ROOT, manifest["architecture_controls"]["persistent_mapping"])
    capacity = manifest["architecture_controls"]["weight_sram_capacity_bytes"]
    frame_count = manifest["workload"]["frames"]
    base_dram = Decimal(manifest["energy_sensitivity"]["base_dram_byte_pj"])
    base_dma = Decimal(manifest["energy_sensitivity"]["base_dma_byte_pj"])
    static_overrides = manifest["static_power_overrides_w"]
    all_runs: dict[str, dict[str, Any]] = {}
    cases: list[dict[str, Any]] = []
    for case in _case_specs(manifest):
        pair_ids = {}
        for arm, mapping in (("no_residency", no_map), ("persistent_residency", resident_map)):
            run_id = f"{case['case_id']}_{arm}"
            run = _invoke(binary, config, mapping, capacity, case["dram"], case["dma"], run_id,
                          output_dir, timeout_seconds, static_overrides,
                          expected_revision, expected_executable_sha256)
            all_runs[run_id] = run
            pair_ids[arm] = run_id
        left, right = (all_runs[pair_ids[name]] for name in ("no_residency", "persistent_residency"))
        case_record: dict[str, Any] = {
            "case_id": case["case_id"], "sensitivity": case["sensitivity"],
            "dram_byte_pj": case["dram"], "dma_byte_pj": case["dma"], "runs": pair_ids,
        }
        if left["status"] == "completed" and right["status"] == "completed":
            try:
                case_record["comparison"] = _pair_statistics(left, right, case["dram"], case["dma"], frame_count)
            except StudyError as error:
                case_record["status"] = "invalid_comparison"
                case_record["failure"] = str(error)
            else:
                case_record["status"] = "complete"
        else:
            case_record["status"] = "partial" if (left["status"] == "completed") != (right["status"] == "completed") else "failed"
            case_record["failure"] = {"no_residency": left["status"], "persistent_residency": right["status"]}
        cases.append(case_record)

    # Re-run the primary pair to check exact same-binary result, provenance, and trace identity.
    repeats = {}
    for arm, mapping in (("no_residency", no_map), ("persistent_residency", resident_map)):
        original = all_runs[f"base_{arm}"]
        repeat_count = manifest["energy_sensitivity"]["additional_repeats_per_base_arm"]
        for repeat_index in range(1, repeat_count + 1):
            repeated_id = f"base_{arm}_repeat{repeat_index}"
            repeated = _invoke(binary, config, mapping, capacity, base_dram, base_dma, repeated_id,
                               output_dir, timeout_seconds, static_overrides,
                               expected_revision, expected_executable_sha256)
            all_runs[repeated_id] = repeated
            if original["status"] == "completed" and repeated["status"] == "completed":
                identical = all(original[key] == repeated[key] for key in ("result_sha256", "provenance_sha256", "trace_sha256"))
                repeats[f"{arm}_repeat{repeat_index}"] = {"status": "identical" if identical else "different",
                            "original_run": original["run_id"], "repeat_run": repeated_id,
                            "result_json_sha256_equal": original["result_sha256"] == repeated["result_sha256"],
                            "provenance_sha256_equal": original["provenance_sha256"] == repeated["provenance_sha256"],
                            "trace_sha256_equal": original["trace_sha256"] == repeated["trace_sha256"]}
            else:
                repeats[f"{arm}_repeat{repeat_index}"] = {"status": "not_comparable", "original_run": original["run_id"], "repeat_run": repeated_id}

    # The insufficient-capacity control is expected to fail before a result or trace is emitted.
    negative_capacity = manifest["architecture_controls"]["capacity_negative_control_bytes"]
    invalid_id = "capacity_negative_control"
    invalid = _invoke(binary, config, resident_map, negative_capacity, base_dram, base_dma,
                      invalid_id, output_dir, timeout_seconds, static_overrides,
                      expected_revision, expected_executable_sha256)
    invalid_result_path = output_dir / "runs" / f"{invalid_id}.json"
    invalid_trace_path = output_dir / "runs" / f"{invalid_id}.trace.csv"
    invalid_expected = (invalid.get("status") == "failed" and invalid.get("process_return_code") == 1
                        and "require 13600 bytes of weight SRAM" in invalid.get("stderr", "")
                        and f"configured capacity is {negative_capacity}" in invalid.get("stderr", "")
                        and not invalid_result_path.exists() and not invalid_trace_path.exists())
    failure_record = {
        "run_id": invalid_id,
        "status": "rejected_as_expected" if invalid_expected else "unexpected_outcome",
        "configured_capacity_bytes": negative_capacity,
        "required_capacity_bytes": manifest["architecture_controls"]["expected_persistent_weight_bytes"],
        "process_return_code": invalid.get("process_return_code"),
        "stderr": invalid.get("stderr", ""),
        "result_json_emitted": invalid_result_path.exists(),
        "trace_csv_emitted": invalid_trace_path.exists(),
    }
    (output_dir / "capacity_negative_control.json").write_bytes(canonical_json(failure_record))

    provenance_states = {
        run_id: run.get("provenance_envelope", {}).get("identity_status", "unavailable")
        for run_id, run in all_runs.items() if run["status"] == "completed"
    }
    provenance_complete = bool(provenance_states) and all(value == "complete" for value in provenance_states.values())
    if not provenance_complete:
        for case in cases:
            if case.get("status") == "complete":
                case["provenance_status"] = "incomplete"
        study_status = "incomplete_provenance"
    elif provenance_complete:
        study_status = "complete"
    else:
        study_status = "incomplete_provenance"
    if any(case.get("status") != "complete" for case in cases) or not invalid_expected or any(v["status"] != "identical" for v in repeats.values()):
        study_status = "incomplete"
    if not provenance_complete:
        study_status = "incomplete_provenance"

    summarized_runs: dict[str, Any] = {}
    for run_id, run in all_runs.items():
        if run["status"] == "completed":
            summarized_runs[run_id] = {"status": "completed", **_run_summary(run)}
            if run["provenance_envelope"]["identity_status"] != "complete":
                summarized_runs[run_id]["status"] = "incomplete_provenance"
        else:
            summarized_runs[run_id] = {key: value for key, value in run.items() if key not in {"result", "provenance", "provenance_envelope"}}

    return {
        "schema_id": "axissim.weight-residency-break-even-results.v1",
        "study_id": manifest["study_id"],
        "study_status": study_status,
        "manifest_sha256": sha256_bytes(canonical_json(manifest)),
        "workload": manifest["workload"],
        "architecture_controls": manifest["architecture_controls"],
        "energy_sensitivity": manifest["energy_sensitivity"],
        "provenance": {
            "complete": provenance_complete,
            "source_revisions": sorted({run.get("provenance", {}).get("source", {}).get("revision", "unknown") for run in all_runs.values() if run["status"] == "completed"}),
            "executable_sha256s": sorted({run.get("provenance", {}).get("executable", {}).get("sha256", "unknown") for run in all_runs.values() if run["status"] == "completed"}),
            "identity_states": provenance_states,
        },
        "cases": cases,
        "reproducibility": repeats,
        "capacity_negative_control": failure_record,
        "runs": summarized_runs,
        "scientific_limits": manifest["scientific_limits"],
    }


CSV_COLUMNS = [
    "case_id", "sensitivity", "status", "dram_byte_pj", "dma_byte_pj",
    "no_residency_dram_bytes", "persistent_residency_dram_bytes", "logical_dram_bytes_saved",
    "logical_dma_payload_bytes_saved_same_transfer_count", "dram_dynamic_energy_saved_pj",
    "dma_dynamic_energy_saved_pj", "total_dynamic_energy_saved_pj", "modeled_static_energy_saved_pj",
    "modeled_it_energy_saved_pj", "max_additional_unpriced_sram_energy_pj_over_observation",
    "max_additional_unpriced_sram_energy_pj_per_completed_frame", "no_residency_makespan_us",
    "persistent_residency_makespan_us", "makespan_reduction_us", "no_residency_mean_latency_us",
    "persistent_residency_mean_latency_us", "mean_latency_reduction_us", "no_residency_p99_latency_us",
    "persistent_residency_p99_latency_us", "p99_latency_reduction_us", "no_residency_deadline_misses",
    "persistent_residency_deadline_misses",
]


def write_summary_csv(study: dict[str, Any], path: pathlib.Path) -> None:
    with path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=CSV_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for case in study["cases"]:
            row = {"case_id": case["case_id"], "sensitivity": case["sensitivity"], "status": case["status"],
                   "dram_byte_pj": case["dram_byte_pj"], "dma_byte_pj": case["dma_byte_pj"]}
            comparison = case.get("comparison")
            if comparison:
                no = comparison["no_residency"]
                resident = comparison["persistent_residency"]
                row.update({
                    "no_residency_dram_bytes": no["total_logical_dram_bytes"],
                    "persistent_residency_dram_bytes": resident["total_logical_dram_bytes"],
                    "no_residency_makespan_us": no["makespan_us"],
                    "persistent_residency_makespan_us": resident["makespan_us"],
                    "no_residency_mean_latency_us": no["mean_latency_us"],
                    "persistent_residency_mean_latency_us": resident["mean_latency_us"],
                    "no_residency_p99_latency_us": no["p99_latency_us"],
                    "persistent_residency_p99_latency_us": resident["p99_latency_us"],
                    "no_residency_deadline_misses": no["deadline_misses"],
                    "persistent_residency_deadline_misses": resident["deadline_misses"],
                })
                for key in CSV_COLUMNS[7:]:
                    if key in comparison:
                        row[key] = comparison[key]
                row["max_additional_unpriced_sram_energy_pj_over_observation"] = comparison[
                    "maximum_additional_unpriced_sram_energy_pj_over_observation"]
                row["max_additional_unpriced_sram_energy_pj_per_completed_frame"] = comparison[
                    "maximum_additional_unpriced_sram_energy_pj_per_completed_frame"]
            writer.writerow({key: decimal_text(value) if isinstance(value, Decimal) else value
                             for key, value in row.items()})


def hypothesis_outcomes(study: dict[str, Any]) -> list[dict[str, str]]:
    complete = [case for case in study["cases"] if case.get("status") == "complete" and "comparison" in case]
    if len(complete) != len(study["cases"]):
        return [{"id": f"H{i}", "outcome": "not_evaluable", "reason": "one or more paired cases are incomplete"} for i in range(1, 6)]
    comparisons = [case["comparison"] for case in complete]
    h1 = all(item["no_residency"]["first_frame_logical_bytes"] == item["persistent_residency"]["first_frame_logical_bytes"] for item in comparisons)
    frames = int(study["workload"]["frames"])
    expected_steady_saving = int(study["architecture_controls"]["expected_persistent_weight_bytes"])
    h2 = all(item["logical_dram_bytes_saved"] == expected_steady_saving * (frames - 1) and
             item["no_residency"]["first_frame_logical_bytes"] == item["persistent_residency"]["first_frame_logical_bytes"] and
             item["no_residency"]["steady_frame_logical_bytes"] - item["persistent_residency"]["steady_frame_logical_bytes"] == expected_steady_saving
             for item in comparisons)
    h3 = all(item["total_dynamic_energy_saved_pj"] == item["dram_dynamic_energy_saved_pj"] + item["dma_dynamic_energy_saved_pj"]
             for item in comparisons)
    h4 = all(item["mean_latency_reduction_us"] >= 0 and item["p99_latency_reduction_us"] >= 0 and
             item["deadline_misses_avoided"] >= 0 for item in comparisons)
    h5 = all(item["maximum_additional_unpriced_sram_energy_pj_over_observation"] > 0 for item in comparisons)
    return [
        {"id": "H1", "outcome": "supported" if h1 else "not_supported", "reason": "first-frame payload equality across all coefficient settings"},
        {"id": "H2", "outcome": "supported" if h2 else "not_supported", "reason": "first-frame, steady-state ledger, and total-run traffic aggregates reconcile with the declared weight reuse"},
        {"id": "H3", "outcome": "supported" if h3 else "not_supported", "reason": "reported dynamic energy reduction reconciles with the DRAM and DMA categories; independent tests check each coefficient against the byte-savings arithmetic"},
        {"id": "H4", "outcome": "supported" if h4 else "not_supported", "reason": "C++ latency and deadline outcomes do not worsen under the matched mapping change"},
        {"id": "H5", "outcome": "supported" if h5 else "not_supported", "reason": "the modeled finite-interval IT difference leaves a positive upper budget for unpriced incremental SRAM energy across the declared sensitivity cases; actual SRAM cost remains unknown"},
    ]


def write_report(study: dict[str, Any], path: pathlib.Path) -> None:
    cases = study["cases"]
    lines = [
        "# M8.0 weight-residency break-even results", "",
        f"Study status: **{study['study_status']}**", "",
        "These are C++ simulator outputs under the declared MNIST-12 profile assumptions. They are not measured hardware latency, memory behavior, or energy.", "",
        "## Paired results", "",
        "| Case | DRAM coefficient (pJ/B) | DMA coefficient (pJ/B) | Logical bytes saved | DRAM energy saved (µJ) | DMA term saved (µJ) | Static energy saved (µJ) | Maximum unpriced SRAM cost over 30 frames (µJ) | Allowance per completed frame (µJ) | Mean latency reduction (µs) | P99 reduction (µs) | Deadline misses avoided |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for case in cases:
        comparison = case.get("comparison")
        if not comparison:
            lines.append(f"| {case['case_id']} | — | — | — | — | — | — | — | — | — | — | — |")
            continue
        to_uj = Decimal(1_000_000)
        values = [
            case["case_id"], decimal_text(case["dram_byte_pj"]), decimal_text(case["dma_byte_pj"]),
            str(comparison["logical_dram_bytes_saved"]),
            decimal_text(comparison["dram_dynamic_energy_saved_pj"] / to_uj),
            decimal_text(comparison["dma_dynamic_energy_saved_pj"] / to_uj),
            decimal_text(comparison["modeled_static_energy_saved_pj"] / to_uj),
            decimal_text(comparison["maximum_additional_unpriced_sram_energy_pj_over_observation"] / to_uj),
            decimal_text(comparison["maximum_additional_unpriced_sram_energy_pj_per_completed_frame"] / to_uj),
            decimal_text(comparison["mean_latency_reduction_us"]), decimal_text(comparison["p99_latency_reduction_us"]),
            str(comparison["deadline_misses_avoided"]),
        ]
        lines.append("| " + " | ".join(values) + " |")

    outcomes = hypothesis_outcomes(study)
    lines += ["", "## Primary pair details", ""]
    base = next((case for case in cases if case["case_id"] == "base"), None)
    if base and base.get("comparison"):
        pair = base["comparison"]
        no = pair["no_residency"]
        resident = pair["persistent_residency"]
        lines += [
            f"- The finite run completed {no['frames_completed']} frames in both arms.",
            f"- First-frame ledger: {no['first_frame_logical_bytes']} B in each arm.",
            f"- C++ steady-state ledger: {no['steady_frame_logical_bytes']} B without residency and {resident['steady_frame_logical_bytes']} B with residency. This is an aggregate accounting field, not a frame-indexed trace.",
            f"- Total logical DRAM payload: {no['total_logical_dram_bytes']} B without residency and {resident['total_logical_dram_bytes']} B with residency.",
            f"- Modeled DMA energy uses the same transfer-byte counter; the {pair['logical_dma_payload_bytes_saved_same_transfer_count']} B DMA payload reduction is not an independent physical bus measurement.",
            f"- Makespan: {no['makespan_us']} µs without residency and {resident['makespan_us']} µs with residency.",
            f"- Mean/P99 frame latency: {no['mean_latency_us']}/{no['p99_latency_us']} µs without residency and {resident['mean_latency_us']}/{resident['p99_latency_us']} µs with residency.",
            f"- Deadline misses: {no['deadline_misses']}/{no['deadline_jobs']} without residency and {resident['deadline_misses']}/{resident['deadline_jobs']} with residency.",
            f"- Static energy difference reported by the model: {decimal_text(pair['modeled_static_energy_saved_pj'] / Decimal(1_000_000))} µJ. Static coefficients in this MNIST profile are configured as zero; this is not a measured physical zero.",
            f"- At the declared {study['energy_sensitivity']['base_dram_byte_pj']} pJ/B DRAM and {study['energy_sensitivity']['base_dma_byte_pj']} pJ/B DMA coefficients, the finite-run incremental unpriced SRAM-energy budget is {decimal_text(pair['maximum_additional_unpriced_sram_energy_pj_over_observation'] / Decimal(1_000_000))} µJ total, or {decimal_text(pair['maximum_additional_unpriced_sram_energy_pj_per_completed_frame'] / Decimal(1_000_000))} µJ per completed frame on average.",
        ]
    else:
        lines.append("The primary pair is incomplete; no paired break-even interpretation is reported.")

    lines += ["", "## Preregistered hypothesis outcomes", ""]
    for outcome in outcomes:
        lines.append(f"- **{outcome['id']}: {outcome['outcome']}** — {outcome['reason']}.")
    negative = study["capacity_negative_control"]
    stored_bytes = int(study["architecture_controls"]["expected_persistent_weight_bytes"])
    capacity_bytes = int(study["architecture_controls"]["weight_sram_capacity_bytes"])
    lines += ["", "## Capacity control", "",
              f"The {negative['configured_capacity_bytes']} B persistent-store control was **{negative['status']}**; it requires {negative['required_capacity_bytes']} B and returned process status {negative['process_return_code']}. No result JSON or trace was emitted for this invalid configuration.",
              f"The accepted configuration declares {capacity_bytes} B for {stored_bytes} B of persistent weights ({capacity_bytes - stored_bytes} B of declared capacity remaining). These are model configuration values, not physical macro area or cost.",
              "", "## Interpretation and limits", "",
              "The break-even amount is a threshold for the sum of all incremental, currently unpriced resident-weight SRAM energy over the matched observation. No SRAM access count, per-access energy, resident-store static-power term, or capacity-energy coefficient is available, so the study cannot say whether real SRAM cost is below the threshold. The 16 KiB capacity is an explicit modeled configuration, not a physically characterized macro.",
              "", "The transfer-byte ledger is logical modeled payload. Existing C++ energy accounting applies both DRAM and DMA coefficients to that same payload count. Sensitivity points vary declared energy coefficients only; they do not represent measured technology uncertainty. No physical DRAM command, power, energy, silicon latency, or hardware-superiority claim follows.", ""]
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def _all_generated_files(output_dir: pathlib.Path) -> list[pathlib.Path]:
    return sorted(path for path in output_dir.rglob("*") if path.is_file() and path.name != "SHA256SUMS.txt")


def write_hash_inventory(output_dir: pathlib.Path) -> None:
    lines = [f"{sha256_file(path)}  {path.relative_to(output_dir).as_posix()}" for path in _all_generated_files(output_dir)]
    (output_dir / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def run_study(binary_arg: str, output_arg: str, manifest_arg: str,
              timeout_seconds: int) -> tuple[int, pathlib.Path]:
    binary = pathlib.Path(binary_arg)
    if not binary.is_absolute():
        binary = (ROOT / binary).resolve()
    if not binary.is_file():
        raise StudyError(f"C++ evaluator does not exist: {binary}")
    revision = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--verify", "HEAD"],
                              capture_output=True, text=True, check=False)
    source_status = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"],
                                   capture_output=True, text=True, check=False)
    if revision.returncode != 0 or source_status.returncode != 0:
        raise StudyError("cannot verify the source revision and working-tree state")
    if source_status.stdout.strip():
        raise StudyError("study execution requires a clean source checkout; use a clean worktree and keep output outside it")
    expected_revision = revision.stdout.strip()
    expected_executable_sha256 = sha256_file(binary)
    manifest_path = pathlib.Path(manifest_arg)
    if not manifest_path.is_absolute():
        manifest_path = (ROOT / manifest_path).resolve()
    manifest, manifest_raw = load_manifest(manifest_path)
    final_dir = pathlib.Path(output_arg)
    if not final_dir.is_absolute():
        final_dir = (ROOT / final_dir).resolve()
    if final_dir.exists():
        raise StudyError(f"refusing to overwrite existing output directory: {final_dir}")
    try:
        final_dir.relative_to(ROOT)
    except ValueError:
        pass
    else:
        raise StudyError("study output must be outside the source checkout to preserve provenance identity")
    final_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(prefix=f".{final_dir.name}.staging-", dir=final_dir.parent))
    try:
        (staging / "manifest.json").write_bytes(manifest_raw)
        study = _run_manifest(manifest, binary, staging, timeout_seconds,
                              expected_revision, expected_executable_sha256)
        study["manifest_sha256"] = sha256_bytes(manifest_raw)
        (staging / "study.json").write_bytes(canonical_json(study))
        write_summary_csv(study, staging / "summary.csv")
        write_report(study, staging / "report.md")
        write_hash_inventory(staging)
        os.replace(staging, final_dir)
    except BaseException:
        if staging.exists():
            # Preserve a recoverable ledger if invocation or report generation fails.
            failed_dir = final_dir.with_name(final_dir.name + ".incomplete")
            if not failed_dir.exists():
                os.replace(staging, failed_dir)
            else:
                shutil.rmtree(staging)
        raise
    return (0 if study["study_status"] == "complete" else 1), final_dir


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="path to the built C++ axissim executable")
    parser.add_argument("--output-dir", required=True, help="new directory for results; existing directories are never overwritten")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST), help="study manifest (defaults to this directory's manifest.json)")
    parser.add_argument("--timeout-seconds", type=int, default=DEFAULT_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    try:
        status, output_dir = run_study(args.binary, args.output_dir, args.manifest,
                                      args.timeout_seconds)
    except (OSError, StudyError, json.JSONDecodeError) as error:
        print(f"M8.0 study error: {error}", file=sys.stderr)
        return 2
    print(f"M8.0 study status: {'complete' if status == 0 else 'incomplete'}")
    print(f"Artifacts: {output_dir}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
