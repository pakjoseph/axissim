#!/usr/bin/env python3
"""Plan, preflight, execute, and reproduce the AxisSim budget-constrained study.

Python owns bounded orchestration and comparison. C++ remains authoritative for
simulation, energy, power, and modeled-budget status.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
from decimal import Decimal, InvalidOperation
from typing import Any, Callable


PLANNING_SCHEMA = "axissim.study-planning-manifest.v1"
STUDY_SCHEMA = "axissim.budget-constrained-study.v1"
ACCOUNTING_MODEL_ID = "axissim-model-accounting-v1"
MNIST_OVERLAY_FIELDS = (
    "static_power_w",
    "npu_static_power_per_mac_w",
    "rvv_static_power_per_lane_w",
    "sram_static_power_per_kib_w",
    "dram_interface_static_power_w",
    "it_power_budget_w",
)
REQUIRED_LIMITS = {
    "max_candidates": "integer",
    "max_task_events_per_candidate": "integer",
    "max_backlog_work_units": "integer",
    "max_retained_trace_bytes": "integer",
    "max_candidate_result_bytes": "integer",
    "max_study_output_bytes": "integer",
    "max_simulated_time_us": "number",
    "max_drain_simulated_us": "number",
    "candidate_wall_timeout_seconds": "number",
}
MIN_CANDIDATE_RESULT_BYTES = 4096
ESTIMATED_TASK_JSONL_LINE_BYTES = 2048
ESTIMATED_TASK_JSONL_HEADER_BYTES = 2048
SUMMARY_CANDIDATE_RESERVE_BYTES = 65536
SUMMARY_GROUP_RESERVE_BYTES = 65536
RESULT_CORE_FIELDS = {
    "schema_id", "candidate_id", "manifest_id", "simulator_build_identity",
    "configuration_identity", "workload_identity", "model_assumption_identity",
    "measurement_protocol_identity", "measurement_protocol",
    "measurement_protocol_unavailable_reason", "effective_configuration",
    "effective_configuration_unavailable_reason", "execution_status",
    "power_budget_status", "configured_it_power_budget_w",
    "configured_it_power_budget_unavailable_reason", "feasibility_label",
    "operating_state", "measurements", "measurements_unavailable_reason",
    "power_accounting_coverage", "power_accounting_coverage_unavailable_reason",
    "trace_export", "censor_reason", "errors",
}
REQUEST_IDENTITY_FIELDS = (
    "candidate_id", "manifest_id", "simulator_build_identity", "configuration_identity",
    "workload_identity", "model_assumption_identity", "measurement_protocol_identity",
)
RUNNER_FAILURE_SCHEMA = "axissim.study-runner-failure.v1"


class StudyError(Exception):
    pass


def _pairs_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise StudyError(f"duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise StudyError(f"non-standard JSON number is forbidden: {value}")


def load_json(path: pathlib.Path) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            parse_int=Decimal,
            parse_float=Decimal,
            parse_constant=_reject_constant,
            object_pairs_hook=_pairs_no_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, InvalidOperation) as error:
        raise StudyError(f"cannot read JSON {path}: {error}") from error


def _canonical_string(value: str) -> str:
    value = unicodedata.normalize("NFC", value)
    if any(0xD800 <= ord(char) <= 0xDFFF for char in value):
        raise StudyError("unpaired Unicode surrogate is not a scalar value")
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _canonical_number(value: Decimal | int) -> str:
    number = value if isinstance(value, Decimal) else Decimal(value)
    if not number.is_finite():
        raise StudyError("non-finite JSON number is forbidden")
    if number == 0:
        return "0"
    try:
        binary = float(number)
    except (OverflowError, ValueError) as error:
        raise StudyError("JSON number is outside binary64 range") from error
    if not math.isfinite(binary) or binary == 0.0:
        raise StudyError("JSON number is outside finite nonzero binary64 range")
    text = format(number, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    if text.startswith("-0") and Decimal(text) == 0:
        return "0"
    return text


def canonical_json(value: Any) -> bytes:
    def encode(item: Any) -> str:
        if item is None:
            return "null"
        if item is True:
            return "true"
        if item is False:
            return "false"
        if isinstance(item, (Decimal, int)) and not isinstance(item, bool):
            return _canonical_number(item)
        if isinstance(item, float):
            if not math.isfinite(item):
                raise StudyError("non-finite JSON number is forbidden")
            return _canonical_number(Decimal(str(item)))
        if isinstance(item, str):
            return _canonical_string(item)
        if isinstance(item, list):
            return "[" + ",".join(encode(element) for element in item) + "]"
        if isinstance(item, dict):
            normalized: dict[str, Any] = {}
            for key, element in item.items():
                if not isinstance(key, str):
                    raise StudyError("canonical JSON object keys must be strings")
                nfc_key = unicodedata.normalize("NFC", key)
                if nfc_key in normalized:
                    raise StudyError(f"object keys collide after Unicode NFC normalization: {key!r}")
                normalized[nfc_key] = element
            keys = sorted(normalized, key=lambda key: key.encode("utf-8"))
            return "{" + ",".join(_canonical_string(key) + ":" + encode(normalized[key]) for key in keys) + "}"
        raise StudyError(f"unsupported canonical JSON value: {type(item).__name__}")

    return encode(value).encode("utf-8")


def digest(value: Any) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def repo_path(root: pathlib.Path, value: str) -> pathlib.Path:
    candidate = pathlib.PurePosixPath(value)
    if candidate.is_absolute() or ".." in candidate.parts or "\\" in value:
        raise StudyError(f"path must be repository-relative POSIX without '..': {value!r}")
    resolved = root.joinpath(*candidate.parts).resolve()
    if root.resolve() not in resolved.parents and resolved != root.resolve():
        raise StudyError(f"path escapes repository: {value!r}")
    if not resolved.is_file():
        raise StudyError(f"input file does not exist: {value}")
    return resolved


def _validate_top_level(manifest: dict[str, Any], final: bool) -> None:
    expected_schema = STUDY_SCHEMA if final else PLANNING_SCHEMA
    if manifest.get("schema_id") != expected_schema:
        raise StudyError(f"unsupported manifest schema; expected {expected_schema}")
    required_top_level = {
        "schema_id", "schema_version", "accounting_model_id", "power_budget_w", "factor_order", "factor_levels",
        "workloads", "groups", "omitted_interactions", "planning_notes",
    }
    if final:
        required_top_level |= {
            "planning_report_path", "factor_selection_rationale", "selection_and_limits_rationale", "limits",
            "limit_rationale", "source_hashes", "trace_policy",
        }
    if set(manifest) != required_top_level:
        missing = sorted(required_top_level - set(manifest))
        unknown = sorted(set(manifest) - required_top_level)
        raise StudyError(f"manifest fields do not match schema; missing={missing}, unknown={unknown}")
    if manifest.get("schema_version") != 1:
        raise StudyError("unsupported manifest schema_version")
    if manifest.get("accounting_model_id") != ACCOUNTING_MODEL_ID:
        raise StudyError(f"accounting_model_id must be {ACCOUNTING_MODEL_ID}")
    if Decimal(str(manifest.get("power_budget_w", "NaN"))) != Decimal("0.23"):
        raise StudyError("the study power budget must remain exactly 0.23 W")
    if not isinstance(manifest.get("factor_order"), list) or not isinstance(manifest.get("workloads"), list):
        raise StudyError("manifest requires factor_order and workloads arrays")
    if not isinstance(manifest.get("groups"), list):
        raise StudyError("manifest requires an explicit groups array")
    factor_ids = manifest["factor_order"]
    if len(factor_ids) != len(set(factor_ids)) or not all(isinstance(item, str) for item in factor_ids):
        raise StudyError("factor_order must contain unique string IDs")
    if set(manifest.get("factor_levels", {})) != set(factor_ids):
        raise StudyError("factor_levels must declare exactly the factor_order IDs")
    workload_ids = [item.get("workload_id") for item in manifest["workloads"]]
    if len(workload_ids) != len(set(workload_ids)):
        raise StudyError("workload_id values must be unique")
    known = set(factor_ids)
    workload_id_set = set(workload_ids)
    group_ids: set[str] = set()
    for group in manifest["groups"]:
        if not isinstance(group, dict) or not isinstance(group.get("group_id"), str):
            raise StudyError("every group needs a string group_id")
        if group["group_id"] in group_ids:
            raise StudyError(f"duplicate group_id: {group['group_id']}")
        group_ids.add(group["group_id"])
        allowed_group_fields = {"group_id", "factors", "workload_ids", "candidate_rows", "comparison_class",
                                "fixed_overrides", "held_constant_factor_assignments", "paired_interaction_id"}
        if set(group) - allowed_group_fields or not {"group_id", "factors", "workload_ids", "candidate_rows", "comparison_class"}.issubset(group):
            raise StudyError(f"group {group['group_id']} has unsupported or missing fields")
        if not isinstance(group.get("workload_ids"), list) or not group["workload_ids"] or not set(group["workload_ids"]).issubset(workload_id_set):
            raise StudyError(f"group {group['group_id']} must explicitly name known workload IDs")
        factors = group.get("factors")
        rows = group.get("candidate_rows")
        if not isinstance(factors, list) or not factors or any(factor not in known for factor in factors):
            raise StudyError(f"group {group['group_id']} has unknown or empty factors")
        if not isinstance(rows, list) or not rows:
            raise StudyError(f"group {group['group_id']} needs explicit candidate_rows")
        for row in rows:
            if not isinstance(row, dict) or set(row) != set(factors):
                raise StudyError(f"group {group['group_id']} row keys must exactly match its factors")
            if len(canonical_json(row)) == 0:
                raise StudyError(f"group {group['group_id']} contains an invalid row")
            if final:
                for factor, value in row.items():
                    if value not in manifest["factor_levels"].get(factor, []):
                        raise StudyError(f"group {group['group_id']} uses unlisted level {factor}={value}")
        row_keys = [canonical_json(row) for row in rows]
        if len(row_keys) != len(set(row_keys)):
            raise StudyError(f"group {group['group_id']} has duplicate rows")
        if group.get("comparison_class") not in ("architecture", "model_sensitivity"):
            raise StudyError(f"group {group['group_id']} has invalid comparison_class")
    for workload in manifest["workloads"]:
        allowed_workload_fields = {
            "workload_id", "family_id", "workload_mode", "base_config_path", "included_config_paths", "source_paths",
            "profile_path", "mapping_path", "fixed_overrides", "pilot_overrides", "pilot_release_stratum_id",
            "base_factor_assignments", "factor_config_keys", "applicability", "power_coverage_expectations",
            "release_strata", "measurement_protocol", "power_overlay", "pilot_power_overlay", "rvv_chunk_stage_elements",
        }
        if set(workload) - allowed_workload_fields or not {"workload_id", "family_id", "workload_mode", "base_config_path",
                "included_config_paths", "profile_path", "mapping_path", "fixed_overrides", "base_factor_assignments",
                "factor_config_keys", "applicability", "power_coverage_expectations", "release_strata",
                "measurement_protocol", "power_overlay"}.issubset(workload):
            raise StudyError(f"workload {workload.get('workload_id')} has unsupported or missing fields")
        if not isinstance(workload.get("release_strata"), list) or not workload["release_strata"]:
            raise StudyError(f"workload {workload.get('workload_id')} needs release_strata")
        stratum_ids = [item.get("release_stratum_id") for item in workload["release_strata"]]
        if len(stratum_ids) != len(set(stratum_ids)):
            raise StudyError(f"workload {workload.get('workload_id')} has duplicate release strata")
        for stratum in workload["release_strata"]:
            if set(stratum) != {"release_stratum_id", "release_schedule", "config_overrides"}:
                raise StudyError(f"workload {workload.get('workload_id')} has an invalid release stratum record")
            schedule = stratum["release_schedule"]
            if schedule.get("kind") == "periodic":
                if set(schedule) != {"kind", "period_us"} or Decimal(str(schedule["period_us"])) <= 0:
                    raise StudyError("periodic release schedule must contain a positive period_us")
            elif schedule.get("kind") == "trace":
                if set(schedule) != {"kind", "path", "provenance_path"}:
                    raise StudyError("trace release schedule must declare path and provenance_path")
            else:
                raise StudyError("release schedule kind must be periodic or trace")
        applicability = workload.get("applicability")
        if not isinstance(applicability, dict):
            raise StudyError(f"workload {workload.get('workload_id')} needs explicit factor applicability")
        if set(applicability) != known or any(value not in ("modeled", "n/a") for value in applicability.values()):
            raise StudyError(f"workload {workload.get('workload_id')} applicability must classify every factor")
        coverage = workload.get("power_coverage_expectations")
        if not isinstance(coverage, dict) or set(coverage) != known:
            raise StudyError(f"workload {workload.get('workload_id')} needs a coverage expectation for every factor")
        if set(workload["measurement_protocol"]) != {"mode", "time_origin_us", "deadline_policy", "percentile_method"}:
            raise StudyError(f"workload {workload.get('workload_id')} has an unsupported measurement protocol schema")
        if workload["measurement_protocol"]["mode"] != "finite" or workload["measurement_protocol"]["time_origin_us"] != 0:
            raise StudyError("M4's selected checked-in workloads must use the finite protocol from model time zero")
        if workload["measurement_protocol"]["percentile_method"] != "linear_n_minus_1_p99":
            raise StudyError("unsupported latency percentile convention")
        overlay = workload.get("power_overlay")
        if not isinstance(overlay, dict) or set(overlay) != {"source_path", "source_sha256", "fields"}:
            raise StudyError(f"workload {workload.get('workload_id')} needs a versioned power_overlay record")
        if not isinstance(overlay["fields"], dict):
            raise StudyError("power_overlay.fields must be an object")
        if workload["workload_mode"] == "profiled_graph":
            expected_overlay = {
                "static_power_w": Decimal("0.05"),
                "npu_static_power_per_mac_w": Decimal("0.00003"),
                "rvv_static_power_per_lane_w": Decimal("0.002"),
                "sram_static_power_per_kib_w": Decimal("0.0001"),
                "dram_interface_static_power_w": Decimal("0.02"),
                "it_power_budget_w": Decimal("0.23"),
            }
            if overlay["source_path"] != "configs/power_budget/manifest.conf" or overlay["fields"] != expected_overlay:
                raise StudyError("the MNIST-12 study-only overlay must exactly match the six documented power fields")
        elif overlay["fields"]:
            raise StudyError("event-scheduler baselines already resolve power terms and must not apply a second overlay")
        if final:
            for key in ("source_hashes", "limits", "limit_rationale", "planning_report_path", "factor_levels"):
                if key not in manifest:
                    raise StudyError(f"final manifest is missing {key}")
            for factor in factor_ids:
                if not isinstance(manifest.get("factor_levels", {}).get(factor), list):
                    raise StudyError(f"final manifest needs explicit factor_levels for {factor}")
            trace_policy = manifest.get("trace_policy")
            if not isinstance(trace_policy, dict) or set(trace_policy) != {
                    "baseline_trace_replay", "frontier_trace_replay", "max_trace_bytes",
                    "preflight_storage_assumes_every_candidate_may_be_on_frontier"}:
                raise StudyError("final manifest needs an explicit trace_policy.max_trace_bytes")
            if trace_policy["baseline_trace_replay"] is not True or trace_policy["frontier_trace_replay"] is not True or trace_policy["preflight_storage_assumes_every_candidate_may_be_on_frontier"] is not True:
                raise StudyError("trace_policy must preserve baseline/frontier traces and worst-case all-candidate storage")
            _positive_limit("trace_policy.max_trace_bytes", trace_policy["max_trace_bytes"], "integer")
            if not isinstance(manifest["limit_rationale"], dict) or set(manifest["limit_rationale"]) != set(REQUIRED_LIMITS):
                raise StudyError("limit_rationale must explain each of the nine required limits")
            if any(not isinstance(value, str) or not value.strip() for value in manifest["limit_rationale"].values()):
                raise StudyError("every limit rationale must be a non-empty string")
            if set(manifest["factor_selection_rationale"]) != known:
                raise StudyError("factor_selection_rationale must explain every declared factor")
            if any(not isinstance(value, str) or not value.strip() for value in manifest["factor_selection_rationale"].values()):
                raise StudyError("every factor selection rationale must be a non-empty string")


def source_paths(manifest: dict[str, Any]) -> list[str]:
    paths: set[str] = set()
    for workload in manifest["workloads"]:
        for key in ("base_config_path", "included_config_paths", "profile_path", "mapping_path", "source_paths"):
            value = workload.get(key)
            if isinstance(value, str) and value:
                paths.add(value)
            elif isinstance(value, list):
                paths.update(path for path in value if path)
        overlay = workload.get("power_overlay", {})
        if overlay.get("source_path"):
            paths.add(overlay["source_path"])
        for stratum in workload["release_strata"]:
            schedule = stratum.get("release_schedule", {})
            if schedule.get("kind") == "trace":
                paths.add(schedule["path"])
                if schedule.get("provenance_path"):
                    paths.add(schedule["provenance_path"])
    return sorted(paths, key=lambda item: item.encode("utf-8"))


def calculate_source_hashes(root: pathlib.Path, paths: list[str]) -> dict[str, str]:
    return {path: file_sha256(repo_path(root, path)) for path in paths}


def build_candidate(manifest: dict[str, Any], workload: dict[str, Any], stratum: dict[str, Any],
                    group: dict[str, Any], row: dict[str, Any], build_identity: str,
                    source_hashes: dict[str, str]) -> dict[str, Any]:
    factor_order = manifest["factor_order"]
    applicable = [factor for factor in factor_order if workload["applicability"][factor] == "modeled"]
    factor_values = dict(workload.get("base_factor_assignments", {}))
    factor_values.update(group.get("held_constant_factor_assignments", {}))
    factor_values.update(row)
    if "release_schedule" in manifest["factor_order"]:
        factor_values["release_schedule"] = stratum["release_stratum_id"]
    factor_values = {factor: factor_values[factor] for factor in factor_order if factor in factor_values and factor in applicable}

    overrides = dict(workload.get("fixed_overrides", {}))
    overrides.update(group.get("fixed_overrides", {}))
    overrides.update(stratum.get("config_overrides", {}))
    config_keys = workload.get("factor_config_keys", {})
    for factor, value in row.items():
        key = config_keys.get(factor)
        if key:
            overrides[key] = value

    included = workload.get("included_config_paths", [])
    included_hashes = {path: source_hashes[path] for path in included}
    base_path = workload["base_config_path"]
    configuration_identity = digest({
        "base_config_path": base_path,
        "base_config_sha256": source_hashes[base_path],
        "included_config_hashes": included_hashes,
        "overrides": overrides,
    })
    release_schedule = dict(stratum["release_schedule"])
    if release_schedule.get("kind") == "trace":
        release_schedule = {"kind": "trace", "path": release_schedule["path"],
                            "sha256": source_hashes[release_schedule["path"]]}
    workload_identity = digest({
        "workload_id": workload["workload_id"],
        "workload_mode": workload["workload_mode"],
        "profile_sha256": source_hashes.get(workload.get("profile_path")) if workload.get("profile_path") else None,
        "mapping_sha256": source_hashes.get(workload.get("mapping_path")) if workload.get("mapping_path") else None,
        "release_stratum_id": stratum["release_stratum_id"],
        "release_schedule": release_schedule,
    })
    overlay = workload.get("power_overlay", {"source_path": None, "source_sha256": None, "fields": {}})
    model_identity = digest({"accounting_model_id": ACCOUNTING_MODEL_ID,
                             "configuration_identity": configuration_identity,
                             "power_overlay": overlay})
    protocol = workload["measurement_protocol"]
    protocol_identity = digest(protocol)
    assignments = [{"factor_id": factor, "value": factor_values[factor]} for factor in factor_order if factor in factor_values]
    candidate_id = digest({
        "manifest_id": digest(manifest),
        "simulator_build_identity": build_identity,
        "workload_identity": workload_identity,
        "configuration_identity": configuration_identity,
        "model_assumption_identity": model_identity,
        "measurement_protocol_identity": protocol_identity,
        "factor_assignments": assignments,
    })
    return {
        "candidate_id": candidate_id,
        "simulator_build_identity": build_identity,
        "workload_id": workload["workload_id"],
        "family_id": workload["family_id"],
        "workload_mode": workload["workload_mode"],
        "release_stratum_id": stratum["release_stratum_id"],
        "release_schedule": release_schedule,
        "group_id": group["group_id"],
        "comparison_class": group["comparison_class"],
        "factor_assignments": assignments,
        "swept_row": {factor: row[factor] for factor in group["factors"]},
        "applicable_factors": applicable,
        "not_applicable_factors": [factor for factor in factor_order if factor not in applicable],
        "declared_power_coverage_expectations": dict(workload.get("power_coverage_expectations", {})),
        "configuration_identity": configuration_identity,
        "workload_identity": workload_identity,
        "model_assumption_identity": model_identity,
        "measurement_protocol_identity": protocol_identity,
        "base_config_path": base_path,
        "configuration_overrides": overrides,
        "power_overlay": overlay,
    }


def build_candidate_request(manifest: dict[str, Any], candidate: dict[str, Any],
                           source_hashes: dict[str, str], retain_full_trace: bool) -> dict[str, Any]:
    """Build the exact M3 request envelope used for M4 byte preflight.

    M4 does not write requests or execute candidates. Keeping this envelope
    builder here lets preflight reserve the exact v1 request size that M5 will
    materialize, instead of mistaking a planning descriptor for an API request.
    """
    workload = next(item for item in manifest["workloads"]
                    if item["workload_id"] == candidate["workload_id"])
    included_hashes = {path: source_hashes[path] for path in workload["included_config_paths"]}
    profile_path = workload.get("profile_path")
    mapping_path = workload.get("mapping_path")
    assignments = {item["factor_id"]: item["value"] for item in candidate["factor_assignments"]}
    swept_factors = {
        factor
        for membership in candidate["group_memberships"]
        for factor in membership["swept_row"]
    }
    factor_coverage = []
    for factor_id in sorted(assignments, key=lambda value: value.encode("utf-8")):
        group_ids = sorted(
            membership["group_id"]
            for membership in candidate["group_memberships"]
            if factor_id in membership["swept_row"]
        )
        treatment = "swept" if factor_id in swept_factors else "held_constant"
        rationale = ("Swept in " + ", ".join(group_ids) + ".") if group_ids else (
            "Held at its manifest-declared candidate assignment."
        )
        factor_coverage.append({
            "factor_id": factor_id,
            "treatment": treatment,
            "evidence_source": "selected manifest factor assignment and group membership",
            "rationale": rationale,
        })
    overlay = candidate["power_overlay"]
    if overlay["source_path"] is not None:
        if source_hashes.get(overlay["source_path"]) != overlay["source_sha256"]:
            raise StudyError(f"power overlay source hash is not in the validated source set: {overlay['source_path']}")
    return {
        "schema_id": "axissim.study-candidate-request.v1",
        "manifest_id": digest(manifest),
        "candidate_id": candidate["candidate_id"],
        "simulator_build_identity": candidate["simulator_build_identity"],
        "configuration_identity": candidate["configuration_identity"],
        "workload_identity": candidate["workload_identity"],
        "model_assumption_identity": candidate["model_assumption_identity"],
        "measurement_protocol_identity": candidate["measurement_protocol_identity"],
        "configuration": {
            "base_config_path": candidate["base_config_path"],
            "base_config_sha256": source_hashes[candidate["base_config_path"]],
            "included_config_hashes": included_hashes,
            "overrides": candidate["configuration_overrides"],
            "expected_it_power_budget_w": manifest["power_budget_w"],
        },
        "workload": {
            "workload_id": candidate["workload_id"],
            "workload_mode": candidate["workload_mode"],
            "profile_path": profile_path,
            "profile_sha256": source_hashes.get(profile_path) if profile_path else None,
            "mapping_path": mapping_path,
            "mapping_sha256": source_hashes.get(mapping_path) if mapping_path else None,
            "release_stratum_id": candidate["release_stratum_id"],
            "release_schedule": candidate["release_schedule"],
        },
        "measurement_protocol": workload["measurement_protocol"],
        "limits": manifest["limits"],
        "power_overlay": overlay,
        "factor_coverage": factor_coverage,
        "trace_policy": {
            "retain_full_trace": retain_full_trace,
            "max_trace_bytes": manifest["trace_policy"]["max_trace_bytes"],
        },
    }


def enumerate_candidates(manifest: dict[str, Any], build_identity: str,
                         source_hashes: dict[str, str]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    candidates_by_id: dict[str, dict[str, Any]] = {}
    memberships: list[dict[str, Any]] = []
    group_ids = {group["group_id"] for group in manifest["groups"]}
    if len(group_ids) != len(manifest["groups"]):
        raise StudyError("duplicate group IDs")
    for group in manifest["groups"]:
        selected_workloads = set(group.get("workload_ids", []))
        if not selected_workloads:
            raise StudyError(f"group {group['group_id']} must explicitly name workload_ids")
        for workload in manifest["workloads"]:
            if workload["workload_id"] not in selected_workloads:
                continue
            missing = [factor for factor in group["factors"] if workload["applicability"][factor] != "modeled"]
            if missing:
                memberships.append({"group_id": group["group_id"], "workload_id": workload["workload_id"],
                                    "status": "not_applicable", "factors": missing})
                continue
            for stratum in workload["release_strata"]:
                for row_index, row in enumerate(group["candidate_rows"]):
                    candidate = build_candidate(manifest, workload, stratum, group, row, build_identity, source_hashes)
                    candidate_id = candidate["candidate_id"]
                    if candidate_id not in candidates_by_id:
                        candidate["group_memberships"] = []
                        candidates_by_id[candidate_id] = candidate
                    candidates_by_id[candidate_id]["group_memberships"].append({
                        "group_id": group["group_id"],
                        "comparison_class": group["comparison_class"],
                        "row_index": row_index,
                        "swept_row": candidate["swept_row"],
                        "declared_power_coverage_expectations": {
                            factor: workload["power_coverage_expectations"][factor] for factor in group["factors"]
                        },
                    })
    # Python dictionaries preserve insertion order on the supported 3.10+
    # runtime. The nested manifest traversal defines candidate order; retaining
    # first occurrence during deduplication matches the study contract.
    candidates = list(candidates_by_id.values())
    return candidates, memberships


def _run(command: list[str], root: pathlib.Path, timeout: float = 60.0) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, cwd=root, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise StudyError(f"command timed out after {timeout:g}s: {command[0]}") from error
    except OSError as error:
        raise StudyError(f"cannot execute {command[0]}: {error}") from error


def _pilot_command(workload: dict[str, Any], stratum: dict[str, Any], binary: pathlib.Path,
                   json_path: pathlib.Path, trace_path: pathlib.Path) -> list[str]:
    command = [str(binary), "run", workload["base_config_path"]]
    overrides = dict(workload.get("fixed_overrides", {}))
    overrides.update(stratum.get("config_overrides", {}))
    overrides.update(workload.get("pilot_overrides", {}))
    if workload.get("pilot_power_overlay"):
        overrides.update(workload["pilot_power_overlay"])
    for key in sorted(overrides, key=lambda item: item.encode("utf-8")):
        command.extend(["--set", f"{key}={overrides[key]}"])
    command.extend(["--json", str(json_path), "--trace", str(trace_path)])
    return command


def _pilot_workloads(manifest: dict[str, Any]) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    result = []
    for workload in manifest["workloads"]:
        preferred = workload.get("pilot_release_stratum_id", "periodic")
        strata = [item for item in workload["release_strata"] if item["release_stratum_id"] == preferred]
        if len(strata) != 1:
            raise StudyError(f"workload {workload['workload_id']} needs exactly one pilot stratum {preferred!r}")
        result.append((workload, strata[0]))
    return result


def plan(root: pathlib.Path, manifest_path: pathlib.Path, binary_path: pathlib.Path,
         report_path: pathlib.Path) -> dict[str, Any]:
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise StudyError("planning manifest root must be an object")
    _validate_top_level(manifest, final=False)
    binary = binary_path.resolve()
    if not binary.is_file():
        raise StudyError(f"simulator binary does not exist: {binary_path}")
    build_identity = file_sha256(binary)
    source_hashes = calculate_source_hashes(root, source_paths(manifest))
    candidates, not_applicable = enumerate_candidates(manifest, build_identity, source_hashes)
    input_validation = validate_workload_inputs(root, binary, manifest)
    invalid_inputs = [entry for entry in input_validation if entry["status"] != "valid"]
    if invalid_inputs:
        raise StudyError(f"planning inputs failed C++ profile validation: {invalid_inputs}")
    validation_rows = validate_candidate_configs(root, binary, candidates)
    validation_by_identity = {row["configuration_identity"]: row for row in validation_rows}
    for candidate in candidates:
        validation = validation_by_identity[candidate["configuration_identity"]]
        candidate["configuration_validation_status"] = validation["status"]
        if validation["status"] != "valid":
            candidate["configuration_validation_error"] = validation["stderr"] or validation["stdout"]

    pilot_records: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="axissim-study-plan-") as temp_dir:
        temp = pathlib.Path(temp_dir)
        for index, (workload, stratum) in enumerate(_pilot_workloads(manifest)):
            result_path = temp / f"pilot-{index}.json"
            trace_path = temp / f"pilot-{index}.csv"
            command = _pilot_command(workload, stratum, binary, result_path, trace_path)
            start = time.perf_counter()
            process = _run(command, root, timeout=120.0)
            elapsed = time.perf_counter() - start
            if process.returncode != 0 or not result_path.is_file() or not trace_path.is_file():
                raise StudyError(
                    f"baseline pilot {workload['workload_id']} failed: exit={process.returncode}; "
                    f"stdout={process.stdout[-500:]!r}; stderr={process.stderr[-500:]!r}"
                )
            result_bytes = result_path.read_bytes()
            trace_bytes = trace_path.read_bytes()
            result = json.loads(result_bytes)
            if result.get("schema_version") != "1.2":
                raise StudyError(f"baseline pilot {workload['workload_id']} returned an unsupported legacy result schema")
            trace_records = max(0, len(trace_bytes.splitlines()) - 1)
            record = {
                "workload_id": workload["workload_id"],
                "family_id": workload["family_id"],
                "release_stratum_id": stratum["release_stratum_id"],
                "simulator_build_identity": build_identity,
                "source_hashes": {path: source_hashes[path] for path in source_paths_for_workload(workload)},
                "legacy_result_schema_version": result.get("schema_version"),
                "legacy_result_sha256": hashlib.sha256(result_bytes).hexdigest(),
                "legacy_result_bytes": len(result_bytes),
                "legacy_trace_sha256": hashlib.sha256(trace_bytes).hexdigest(),
                "legacy_trace_bytes": len(trace_bytes),
                "legacy_trace_record_count": trace_records,
                "legacy_max_pending_tasks": result.get("qos", {}).get("max_pending_tasks"),
                "pilot_wall_seconds": elapsed,
                "pilot_process_exit_status": process.returncode,
                "pilot_structured_result_status": "available_schema_1.2",
                "pilot_command": relative_command(command, root, binary),
            }
            pilot_records.append(record)

    group_summary = summarize_groups(manifest, candidates, not_applicable)
    invalid_rows = [row for row in validation_rows if row["status"] != "valid"]
    stable_content = {
        "schema_id": "axissim.study-plan-report.v1",
        "planning_manifest_identity": digest(manifest),
        "simulator_build_identity": build_identity,
        "source_hashes": source_hashes,
        "candidate_count_unique": len(candidates),
        "candidate_count_before_deduplication": sum(len(group["candidate_rows"]) *
            sum(len(w["release_strata"]) for w in manifest["workloads"]
                if w["workload_id"] in group.get("workload_ids", [])) for group in manifest["groups"]),
        "candidate_validation": validation_rows,
        "input_validation": input_validation,
        "non_applicable_groups": not_applicable,
        "group_summary": group_summary,
        "candidates": candidates,
        "baseline_pilots_identity": [{key: value for key, value in pilot.items()
                                       if key not in ("pilot_wall_seconds", "pilot_command")}
                                     for pilot in pilot_records],
    }
    report = dict(stable_content)
    report["plan_id"] = digest(stable_content)
    report["status"] = "planned" if not invalid_rows else "planned_with_invalid_rows"
    report["host_diagnostics"] = {
        "platform": platform.platform(),
        "python_version": platform.python_version(),
        "pilots": pilot_records,
        "planning_projection": planning_storage_estimate(candidates, manifest["workloads"], pilot_records),
        "timing_note": "Pilot wall times are host-side estimates, not simulated hardware metrics or statistical confidence intervals.",
    }
    report["preflight_estimator"] = {
        "first_pass_plus_trace_replay_factor": Decimal("2"),
        "result_storage_basis": "max per-workload legacy schema-1.2 pilot result bytes",
        "trace_storage_basis": "2,048-byte task-JSONL line allowance times estimated task rows plus a 2,048-byte header; task JSONL trace is not sampled by planning pilots",
        "summary_reserve_formula": "4096 + 1024*candidate_count + 512*group_membership_count bytes",
        "task_event_estimate": "8*legacy_task_trace_records + 4*scheduled_frame_units; conservative planning estimate, not observed observer counter",
        "backlog_estimate": "scheduled finite work-unit count; an upper bound, not an observed queue snapshot",
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_bytes(canonical_json(report) + b"\n")
    return report


def source_paths_for_workload(workload: dict[str, Any]) -> list[str]:
    paths = {workload["base_config_path"], *workload.get("included_config_paths", []),
             *workload.get("source_paths", [])}
    if workload.get("power_overlay", {}).get("source_path"):
        paths.add(workload["power_overlay"]["source_path"])
    for key in ("profile_path", "mapping_path"):
        if workload.get(key):
            paths.add(workload[key])
    for stratum in workload.get("release_strata", []):
        schedule = stratum.get("release_schedule", {})
        if schedule.get("kind") == "trace":
            paths.add(schedule["path"])
            if schedule.get("provenance_path"):
                paths.add(schedule["provenance_path"])
    return sorted(paths, key=lambda item: item.encode("utf-8"))


def relative_command(command: list[str], root: pathlib.Path, binary: pathlib.Path) -> list[str]:
    result = list(command)
    result[0] = root_relative_or_name(binary, root)
    for index, item in enumerate(result):
        if item.endswith(".json") and pathlib.Path(item).is_absolute():
            result[index] = "<RESULT_JSON>"
        elif item.endswith(".csv") and pathlib.Path(item).is_absolute():
            result[index] = "<TRACE_CSV>"
    return result


def root_relative_or_name(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.name


def validate_candidate_configs(root: pathlib.Path, binary: pathlib.Path,
                               candidates: list[dict[str, Any]],
                               validator: Callable[[list[str], pathlib.Path], subprocess.CompletedProcess[str]] | None = None
                               ) -> list[dict[str, Any]]:
    run = validator or (lambda argv, cwd: _run(argv, cwd))
    unique: dict[str, dict[str, Any]] = {}
    for candidate in candidates:
        key = digest({"base_config_path": candidate["base_config_path"],
                      "overrides": candidate["configuration_overrides"]})
        unique.setdefault(key, candidate)
    records = []
    for key in sorted(unique):
        candidate = unique[key]
        command = [str(binary), "validate", candidate["base_config_path"]]
        for config_key in sorted(candidate["configuration_overrides"], key=lambda item: item.encode("utf-8")):
            command.extend(["--set", f"{config_key}={candidate['configuration_overrides'][config_key]}"])
        process = run(command, root)
        stdout = process.stdout.strip()
        stderr = process.stderr.strip()
        valid = process.returncode == 0 and stdout.startswith("Config is valid:")
        records.append({
            "configuration_identity": candidate["configuration_identity"],
            "workload_id": candidate["workload_id"],
            "return_code": process.returncode,
            "structured_status": "valid" if valid else "invalid",
            "status": "valid" if valid else "invalid",
            "stdout": stdout[:512],
            "stderr": stderr[:512],
        })
    return records


def validate_workload_inputs(root: pathlib.Path, binary: pathlib.Path,
                             manifest: dict[str, Any]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for workload in manifest["workloads"]:
        if not workload.get("profile_path"):
            continue
        key = (workload["profile_path"], workload.get("mapping_path", ""))
        if key in seen:
            continue
        seen.add(key)
        command = [str(binary), "profile-validate", key[0]]
        if key[1]:
            command.append(key[1])
        process = _run(command, root)
        valid = process.returncode == 0 and process.stdout.startswith("Workload profile is valid:")
        records.append({
            "profile_path": key[0],
            "mapping_path": key[1] or None,
            "return_code": process.returncode,
            "structured_status": "valid" if valid else "invalid",
            "status": "valid" if valid else "invalid",
            "stdout": process.stdout.strip()[:512],
            "stderr": process.stderr.strip()[:512],
        })
    return records


def summarize_groups(manifest: dict[str, Any], candidates: list[dict[str, Any]],
                     not_applicable: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    workload_lookup = {workload["workload_id"]: workload for workload in manifest["workloads"]}
    for group in manifest["groups"]:
        for workload_id in group["workload_ids"]:
            workload = workload_lookup[workload_id]
            for stratum in workload["release_strata"]:
                release_stratum_id = stratum["release_stratum_id"]
                members = [candidate for candidate in candidates
                           if candidate["workload_id"] == workload_id and
                           candidate["release_stratum_id"] == release_stratum_id and
                           any(membership["group_id"] == group["group_id"] for membership in candidate["group_memberships"])]
                result.append({
                    "group_id": group["group_id"],
                    "comparison_class": group["comparison_class"],
                    "workload_id": workload_id,
                    "release_stratum_id": release_stratum_id,
                    "factors": group["factors"],
                    "candidate_rows": group["candidate_rows"],
                    "unique_candidate_count": len({candidate["candidate_id"] for candidate in members}),
                    "candidate_ids": sorted({candidate["candidate_id"] for candidate in members}),
                    "candidate_rows_are_explicit": True,
                    "cross_workload_or_release_comparison": False,
                })
    for entry in not_applicable:
        workload = workload_lookup[entry["workload_id"]]
        group = next(item for item in manifest["groups"] if item["group_id"] == entry["group_id"])
        for stratum in workload["release_strata"]:
            result.append({
                "group_id": entry["group_id"],
                "comparison_class": group["comparison_class"],
                "workload_id": entry["workload_id"],
                "release_stratum_id": stratum["release_stratum_id"],
                "factors": group["factors"],
                "candidate_rows": group["candidate_rows"],
                "unique_candidate_count": 0,
                "candidate_ids": [],
                "candidate_rows_are_explicit": True,
                "cross_workload_or_release_comparison": False,
                "status": "not_applicable",
                "not_applicable_factors": entry["factors"],
            })
    return result


def estimated_task_rows(candidate: dict[str, Any], workload: dict[str, Any], pilot: dict[str, Any]) -> int:
    frame_count = int(workload.get("pilot_overrides", {}).get("frames", workload.get("fixed_overrides", {}).get("frames", 30)))
    task_rows = int(pilot["legacy_trace_record_count"])
    assignments = {entry["factor_id"]: entry["value"] for entry in candidate["factor_assignments"]}
    chunk = int(assignments.get("rvv_chunk_elements", 0))
    stage_lengths = workload.get("rvv_chunk_stage_elements", [])
    if chunk > 0 and stage_lengths:
        chunked_stage_tasks = sum((int(length) + chunk - 1) // chunk for length in stage_lengths)
        task_rows += frame_count * (chunked_stage_tasks - len(stage_lengths))
    return task_rows


def planning_storage_estimate(candidates: list[dict[str, Any]], workloads: list[dict[str, Any]],
                              pilots: list[dict[str, Any]]) -> dict[str, Any]:
    workload_by_id = {item["workload_id"]: item for item in workloads}
    pilot_by_id = {item["workload_id"]: item for item in pilots}
    count_by_id: dict[str, int] = {}
    wall_seconds = Decimal(0)
    task_records = 0
    task_events = 0
    trace_bytes = 0
    max_events = 0
    max_trace = 0
    for candidate in candidates:
        workload_id = candidate["workload_id"]
        count_by_id[workload_id] = count_by_id.get(workload_id, 0) + 1
        workload = workload_by_id[workload_id]
        pilot = pilot_by_id[workload_id]
        rows = estimated_task_rows(candidate, workload, pilot)
        frame_count = int(workload.get("pilot_overrides", {}).get("frames", workload.get("fixed_overrides", {}).get("frames", 30)))
        event_count = 8 * rows + 4 * frame_count
        trace_size = ESTIMATED_TASK_JSONL_HEADER_BYTES + rows * ESTIMATED_TASK_JSONL_LINE_BYTES
        task_records += rows
        task_events += event_count
        trace_bytes += trace_size
        max_events = max(max_events, event_count)
        max_trace = max(max_trace, trace_size)
    for workload_id, count in count_by_id.items():
        wall_seconds += Decimal(str(pilot_by_id[workload_id]["pilot_wall_seconds"])) * count * Decimal(2)
    result_bytes = len(candidates) * max(MIN_CANDIDATE_RESULT_BYTES,
        8 * max((int(pilot["legacy_result_bytes"]) for pilot in pilots), default=0))
    descriptor_bytes = _candidate_descriptor_bytes(candidates)
    membership_count = sum(len(candidate["group_memberships"]) for candidate in candidates)
    summary_bytes = 4096 + 1024 * len(candidates) + 512 * membership_count
    return {
        "unique_candidate_count_including_invalid_planning_rows": len(candidates),
        "host_wall_seconds_including_one_worst_case_trace_replay_per_candidate": wall_seconds,
        "host_estimate_is_not_simulated_hardware_performance": True,
        "estimated_max_task_events_per_candidate": max_events,
        "estimated_task_events_total": task_events,
        "estimated_task_trace_records_total": task_records,
        "task_event_estimate_method": "8*estimated task records + 4*scheduled frames; RVV chunk task count expands from declared stage lengths",
        "task_trace_byte_estimate": "2,048 bytes per task JSONL line plus a 2,048-byte header per candidate; conservative estimate, not measured trace size",
        "estimated_trace_bytes_if_every_candidate_retained": trace_bytes,
        "estimated_result_bytes_if_every_candidate_retained": result_bytes,
        "candidate_descriptor_bytes": descriptor_bytes,
        "candidate_request_artifacts_included": False,
        "summary_reserve_bytes": summary_bytes,
        "estimated_total_storage_bytes_excluding_unfinalized_candidate_requests": (
            trace_bytes + result_bytes + descriptor_bytes + summary_bytes
        ),
        "per_workload_unique_candidate_count": count_by_id,
    }


def _positive_limit(name: str, value: Any, kind: str) -> Decimal | int:
    if isinstance(value, bool):
        raise StudyError(f"limit {name} must be positive {kind}")
    try:
        decimal_value = value if isinstance(value, Decimal) else Decimal(str(value))
    except (InvalidOperation, ValueError) as error:
        raise StudyError(f"limit {name} must be positive {kind}") from error
    if not decimal_value.is_finite() or decimal_value <= 0:
        raise StudyError(f"limit {name} must be finite and positive")
    if kind == "integer":
        if decimal_value != decimal_value.to_integral_value():
            raise StudyError(f"limit {name} must be a positive integer")
        return int(decimal_value)
    return decimal_value


def _candidate_descriptor_bytes(candidates: list[dict[str, Any]]) -> int:
    return sum(len(canonical_json(candidate)) + 1 for candidate in candidates)


def preflight(root: pathlib.Path, manifest_path: pathlib.Path, binary_path: pathlib.Path,
              report_path: pathlib.Path, plan_report_path: pathlib.Path,
              validator: Callable[[list[str], pathlib.Path], subprocess.CompletedProcess[str]] | None = None
              ) -> dict[str, Any]:
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise StudyError("final manifest root must be an object")
    _validate_top_level(manifest, final=True)
    binary = binary_path.resolve()
    if not binary.is_file():
        raise StudyError(f"simulator binary does not exist: {binary_path}")
    build_identity = file_sha256(binary)
    current_hashes = calculate_source_hashes(root, source_paths(manifest))
    declared_hashes = manifest["source_hashes"]
    if current_hashes != declared_hashes:
        changed = sorted(set(current_hashes) | set(declared_hashes))
        changed = [path for path in changed if current_hashes.get(path) != declared_hashes.get(path)]
        raise StudyError("source hash mismatch: " + ", ".join(changed))
    declared_plan_report = repo_path(root, manifest["planning_report_path"])
    supplied_plan_report = plan_report_path.resolve()
    if supplied_plan_report != declared_plan_report:
        raise StudyError("--plan-report path differs from planning_report_path in final manifest")
    planning_report_file = declared_plan_report
    planning_report = load_json(planning_report_file)
    if planning_report.get("schema_id") != "axissim.study-plan-report.v1":
        raise StudyError("planning report has unsupported schema")
    if planning_report.get("simulator_build_identity") != build_identity:
        raise StudyError("planning report simulator build identity differs from current binary")
    if planning_report.get("source_hashes") != current_hashes:
        raise StudyError("planning report input hashes differ from final manifest inputs")

    candidates, not_applicable = enumerate_candidates(manifest, build_identity, current_hashes)
    supplied_limits = manifest["limits"]
    if not isinstance(supplied_limits, dict) or set(supplied_limits) != set(REQUIRED_LIMITS):
        missing = sorted(set(REQUIRED_LIMITS) - set(supplied_limits or {}))
        extra = sorted(set(supplied_limits or {}) - set(REQUIRED_LIMITS))
        raise StudyError(f"limits must define exactly all nine fields; missing={missing}, extra={extra}")
    limits = {key: _positive_limit(key, supplied_limits[key], kind) for key, kind in REQUIRED_LIMITS.items()}

    planning_pilots = {item["workload_id"]: item for item in planning_report["host_diagnostics"]["pilots"]}
    candidate_counts: dict[str, int] = {}
    for candidate in candidates:
        candidate_counts[candidate["workload_id"]] = candidate_counts.get(candidate["workload_id"], 0) + 1
    pilot_by_family = {workload["workload_id"]: workload for workload in manifest["workloads"]}
    projected_wall_seconds = Decimal(0)
    projected_trace_bytes = 0
    projected_result_bytes = 0
    max_legacy_result = 0
    estimated_task_events = 0
    estimated_trace_records = 0
    max_task_events_per_candidate = 0
    max_task_trace_bytes_per_candidate = 0
    estimated_backlog = 0
    for workload_id, count in sorted(candidate_counts.items()):
        pilot = planning_pilots.get(workload_id)
        if not pilot:
            raise StudyError(f"planning report has no pilot for {workload_id}")
        workload = pilot_by_family[workload_id]
        frames = int(workload.get("pilot_overrides", {}).get("frames", workload.get("fixed_overrides", {}).get("frames", 30)))
        projected_wall_seconds += Decimal(str(pilot["pilot_wall_seconds"])) * count * Decimal(2)
        projected_result_bytes += int(pilot["legacy_result_bytes"]) * count
        max_legacy_result = max(max_legacy_result, int(pilot["legacy_result_bytes"]))
        estimated_backlog = max(estimated_backlog, frames)
        workload_candidates = [candidate for candidate in candidates if candidate["workload_id"] == workload_id]
        for candidate in workload_candidates:
            task_rows = estimated_task_rows(candidate, workload, pilot)
            event_estimate = 8 * task_rows + 4 * frames
            trace_estimate = ESTIMATED_TASK_JSONL_HEADER_BYTES + task_rows * ESTIMATED_TASK_JSONL_LINE_BYTES
            estimated_task_events += event_estimate
            estimated_trace_records += task_rows
            max_task_events_per_candidate = max(max_task_events_per_candidate, event_estimate)
            max_task_trace_bytes_per_candidate = max(max_task_trace_bytes_per_candidate, trace_estimate)
            projected_trace_bytes += trace_estimate
    workload_by_id = {item["workload_id"]: item for item in manifest["workloads"]}
    comparison_group_instances = sum(
        len(group["workload_ids"])
        * sum(len(workload_by_id[workload_id]["release_strata"]) for workload_id in group["workload_ids"])
        for group in manifest["groups"]
    )
    summary_reserve = (
        4096
        + SUMMARY_CANDIDATE_RESERVE_BYTES * len(candidates)
        + SUMMARY_GROUP_RESERVE_BYTES * comparison_group_instances
    )
    request_bytes_first_pass = sum(
        len(canonical_json(build_candidate_request(manifest, candidate, current_hashes, False)))
        for candidate in candidates
    )
    request_bytes_trace_replay = sum(
        len(canonical_json(build_candidate_request(manifest, candidate, current_hashes, True)))
        for candidate in candidates
    )
    request_bytes = request_bytes_first_pass + request_bytes_trace_replay
    result_reserve = len(candidates) * int(limits["max_candidate_result_bytes"])
    trace_reserve = projected_trace_bytes
    projected_output_bytes = request_bytes + result_reserve + trace_reserve + summary_reserve
    planning_full_count = int(planning_report["candidate_count_unique"])

    cap_checks = {
        "max_candidates": {"required": len(candidates), "limit": limits["max_candidates"], "pass": len(candidates) <= limits["max_candidates"]},
        "max_task_events_per_candidate": {"required_estimated_max": max_task_events_per_candidate,
                                          "limit": limits["max_task_events_per_candidate"]},
        "max_backlog_work_units": {"required_estimated_max": estimated_backlog, "limit": limits["max_backlog_work_units"]},
        "max_retained_trace_bytes": {"required_worst_case": trace_reserve, "limit": limits["max_retained_trace_bytes"]},
        "max_trace_bytes_per_candidate": {"required_estimated_max": max_task_trace_bytes_per_candidate,
                                          "limit": manifest.get("trace_policy", {}).get("max_trace_bytes", 0)},
        "max_candidate_result_bytes": {"required_estimated_max": max(MIN_CANDIDATE_RESULT_BYTES, 8 * max_legacy_result),
                                        "minimum_contract_bytes": MIN_CANDIDATE_RESULT_BYTES,
                                        "estimate_method": "8x largest schema-1.2 baseline result size; conservative storage estimate, not an observed study-result size",
                                        "limit": limits["max_candidate_result_bytes"]},
        "max_study_output_bytes": {"required_worst_case": projected_output_bytes, "limit": limits["max_study_output_bytes"]},
        "max_simulated_time_us": {"required_workload_max": max(int(w.get("pilot_overrides", {}).get("frames", 30)) * int(w.get("pilot_overrides", {}).get("frame_period_us", 33333)) + int(w.get("pilot_overrides", {}).get("deadline_us", 33333)) for w in manifest["workloads"]),
                                   "limit": limits["max_simulated_time_us"]},
        "max_drain_simulated_us": {"required_workload_minimum": 1, "limit": limits["max_drain_simulated_us"]},
        "candidate_wall_timeout_seconds": {"required_projected_max_pilot": max((Decimal(str(item["pilot_wall_seconds"])) for item in planning_pilots.values()), default=Decimal(0)),
                                           "limit": limits["candidate_wall_timeout_seconds"]},
    }
    violations: list[str] = []
    for name, check in cap_checks.items():
        if "pass" in check:
            if not check["pass"]:
                violations.append(name)
            continue
        estimate_key = next((key for key in check if key.startswith("required") or key.startswith("minimum_contract")), None)
        if estimate_key is None:
            continue
        estimate = check[estimate_key]
        if isinstance(estimate, Decimal):
            okay = estimate <= check["limit"]
        else:
            okay = estimate <= check["limit"]
        check["pass"] = okay
        if not okay:
            violations.append(name)
    result = {
        "schema_id": "axissim.study-preflight-report.v1",
        "manifest_identity": digest(manifest),
        "planning_report_identity": planning_report.get("plan_id"),
        "simulator_build_identity": build_identity,
        "status": "rejected" if violations else "ready",
        "structured_status": "rejected" if violations else "ready",
        "exit_code": 2 if violations else 0,
        "rejection_reasons": violations,
        "candidate_count": len(candidates),
        "planning_full_candidate_count": planning_full_count,
        "candidate_count_is_bounded_by_explicit_manifest": True,
        "limits": limits,
        "cap_checks": cap_checks,
        "estimate": {
            "projected_candidate_wall_seconds_including_full_trace_replay": projected_wall_seconds,
            "projected_host_seconds_are_not_simulated_metrics": True,
            "candidate_request_bytes_exact_worst_case": request_bytes,
            "first_pass_request_bytes_exact": request_bytes_first_pass,
            "worst_case_trace_replay_request_bytes_exact": request_bytes_trace_replay,
            "request_size_method": "canonical M3 v1 request-envelope bytes without a trailing newline; one no-trace request and one full-trace replay request reserved per candidate",
            "result_bytes_worst_case_from_declared_candidate_cap": result_reserve,
            "legacy_pilot_result_bytes_observed": projected_result_bytes,
            "retained_trace_bytes_worst_case": trace_reserve,
            "estimated_task_trace_records_worst_case": estimated_trace_records,
            "estimated_max_trace_bytes_per_candidate": max_task_trace_bytes_per_candidate,
            "summary_reserve_bytes": summary_reserve,
            "summary_reserve_method": (
                "4,096-byte fixed envelope plus 65,536 bytes per unique candidate and 65,536 bytes per declared group/workload/release-stratum instance; "
                "conservative space for the compact summary, trace index, and group/frontier CSVs"
            ),
            "total_output_bytes_worst_case": projected_output_bytes,
            "estimated_task_events_total": estimated_task_events,
            "estimated_backlog_work_units_upper_bound": estimated_backlog,
            "task_event_estimate_method": "baseline legacy task records plus deterministic RVV chunk expansion, then 8*estimated task records + 4*scheduled frame units; estimate only",
            "trace_estimate_method": "2,048-byte conservative line allowance per estimated task record plus 2,048-byte header; JSONL trace is not measured by plan pilots",
            "backlog_estimate_method": "all scheduled finite work units simultaneously outstanding; conservative bound, not an observed queue measurement",
        },
        "per_workload_candidate_counts": candidate_counts,
        "non_applicable_groups": not_applicable,
        "groups": summarize_groups(manifest, candidates, not_applicable),
        "candidates": candidates,
        "input_validation": [],
        "candidate_validation": [],
        "candidate_simulations_launched": 0,
        "study_evaluate_invocations": 0,
        "power_status_owner": "C++ study-evaluate; no power values are calculated during preflight",
        "power_coverage_note": "Coverage fields here are manifest expectations; C++ candidate results determine resolved coverage and numeric budget status.",
    }
    # The cap decision must precede every per-candidate C++ validation process.
    if not violations:
        result["input_validation"] = validate_workload_inputs(root, binary, manifest)
        invalid_inputs = [entry for entry in result["input_validation"] if entry["status"] != "valid"]
        if invalid_inputs:
            result["status"] = "rejected"
            result["structured_status"] = "rejected"
            result["exit_code"] = 2
            result["rejection_reasons"] = ["invalid_workload_profile_or_mapping"]
            result["invalid_inputs"] = invalid_inputs
        else:
            result["candidate_validation"] = validate_candidate_configs(root, binary, candidates, validator)
        invalid = [entry for entry in result["candidate_validation"] if entry["status"] != "valid"]
        if invalid:
            result["status"] = "rejected"
            result["structured_status"] = "rejected"
            result["exit_code"] = 2
            result["rejection_reasons"] = ["invalid_candidate_configuration"]
            result["invalid_candidate_configurations"] = invalid
        validation_by_identity = {row["configuration_identity"]: row for row in result["candidate_validation"]}
        if validation_by_identity:
            for candidate in candidates:
                validation = validation_by_identity[candidate["configuration_identity"]]
                candidate["configuration_validation_status"] = validation["status"]
                if validation["status"] != "valid":
                    candidate["configuration_validation_error"] = validation["stderr"] or validation["stdout"]
        else:
            for candidate in candidates:
                candidate["configuration_validation_status"] = "not_validated_input_error"
    else:
        for candidate in candidates:
            candidate["configuration_validation_status"] = "not_run_cap_rejected"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_bytes(canonical_json(result) + b"\n")
    return result


def bounded_text(value: str, limit: int = 256) -> str:
    raw = value.encode("utf-8", errors="replace")[:limit]
    return raw.decode("utf-8", errors="ignore")


def atomic_write(path: pathlib.Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_bytes(content)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def runner_failure_record(candidate_id: str, build_identity: str, runner_status: str,
                          failure_reason: str, message: str) -> dict[str, Any]:
    if runner_status not in ("failed", "censored") or failure_reason not in ("evaluator_crash", "candidate_wall_timeout"):
        raise StudyError("invalid runner-failure status")
    return {
        "schema_id": RUNNER_FAILURE_SCHEMA,
        "candidate_id": candidate_id,
        "simulator_build_identity": build_identity,
        "runner_status": runner_status,
        "failure_reason": failure_reason,
        "message": bounded_text(message),
    }


def validate_candidate_result(result: Any, request: dict[str, Any], return_code: int,
                              result_bytes: int, max_result_bytes: int) -> dict[str, Any]:
    if result_bytes > max_result_bytes:
        raise StudyError("candidate result exceeds max_candidate_result_bytes")
    if not isinstance(result, dict):
        raise StudyError("candidate result root is not an object")
    keys = set(result)
    if not RESULT_CORE_FIELDS.issubset(keys) or keys - RESULT_CORE_FIELDS - {"extensions"}:
        raise StudyError("candidate result fields differ from result contract v1")
    if result["schema_id"] != "axissim.study-candidate-result.v1":
        raise StudyError("candidate result schema is unsupported")
    for identity in REQUEST_IDENTITY_FIELDS:
        if result.get(identity) != request[identity]:
            raise StudyError(f"candidate result identity mismatch: {identity}")
    status = result.get("execution_status")
    expected_exit = {"completed": 0, "invalid": 2, "failed": 1, "censored": 1}
    if status not in expected_exit:
        raise StudyError("candidate result execution_status is invalid")
    if return_code != expected_exit[status]:
        raise StudyError("evaluator process exit code disagrees with structured execution_status")
    if result.get("power_budget_status") not in ("pass", "fail", "not_evaluated"):
        raise StudyError("candidate result power_budget_status is invalid")
    if result.get("operating_state") not in ("not_applicable", "no_growth_observed", "backlog_growing", "unknown"):
        raise StudyError("candidate result operating_state is invalid")
    expected_budget = request["configuration"]["expected_it_power_budget_w"]
    configured_budget = result.get("configured_it_power_budget_w")
    if configured_budget is not None and float(configured_budget) != float(expected_budget):
        # Compare the resolved configuration values in the C++ binary64 domain;
        # full-precision JSON may spell the same double as 0.23000000000000001.
        raise StudyError("resolved candidate budget differs from declared 0.23 W budget")
    if status == "completed":
        if not isinstance(result.get("measurements"), dict) or result.get("power_budget_status") not in ("pass", "fail"):
            raise StudyError("completed candidate is missing measurements or C++ budget status")
        if not isinstance(result.get("power_accounting_coverage"), dict):
            raise StudyError("completed candidate is missing C++ power coverage")
        if result.get("measurements_unavailable_reason") is not None:
            raise StudyError("completed candidate reports unavailable measurements")
    if status == "censored" and not isinstance(result.get("measurements"), dict):
        raise StudyError("censored result must preserve partial measurement counters")
    if status in ("invalid", "failed") and result.get("measurements") is not None:
        # M3 runtime failures are allowed to contain null measurements only;
        # invalid and compact result-limit fallbacks also contain no metrics.
        raise StudyError("invalid or failed candidate unexpectedly carries measurements")
    if not isinstance(result.get("errors"), list):
        raise StudyError("candidate result errors field is not an array")
    if not isinstance(result.get("trace_export"), dict):
        raise StudyError("candidate result trace_export field is not an object")
    # Exercise the canonical encoder to reject non-finite or out-of-binary64 JSON values.
    canonical_json(result)
    return result


def invoke_candidate(root: pathlib.Path, binary: pathlib.Path, request_path: pathlib.Path,
                     result_path: pathlib.Path, trace_path: pathlib.Path | None,
                     timeout_seconds: float, candidate_id: str, build_identity: str,
                     max_result_bytes: int,
                     process_runner: Callable[..., subprocess.CompletedProcess[str]] | None = None
                     ) -> tuple[dict[str, Any], dict[str, Any]]:
    command = [str(binary), "study-evaluate", "--request", str(request_path), "--result", str(result_path)]
    if trace_path is not None:
        command.extend(["--trace", str(trace_path)])
    execute = process_runner or subprocess.run
    started = time.perf_counter()
    return_code: int | None = None
    stdout = ""
    stderr = ""
    try:
        process = execute(command, cwd=root, capture_output=True, text=True,
                          timeout=timeout_seconds, check=False)
        return_code = process.returncode
        stdout = process.stdout or ""
        stderr = process.stderr or ""
    except subprocess.TimeoutExpired as error:
        message = f"candidate exceeded wall timeout of {timeout_seconds:g} seconds"
        if error.stderr:
            detail = error.stderr.decode("utf-8", errors="replace") if isinstance(error.stderr, bytes) else error.stderr
            message += ": " + detail
        try:
            result_path.unlink()
        except FileNotFoundError:
            pass
        record = runner_failure_record(candidate_id, build_identity, "censored", "candidate_wall_timeout", message)
        return record, {"return_code": None, "elapsed_wall_seconds": Decimal(str(time.perf_counter() - started)),
                        "stdout": "", "stderr": bounded_text(message), "runner_failure": True}
    except OSError as error:
        record = runner_failure_record(candidate_id, build_identity, "failed", "evaluator_crash", str(error))
        return record, {"return_code": None, "elapsed_wall_seconds": Decimal(str(time.perf_counter() - started)),
                        "stdout": "", "stderr": bounded_text(str(error)), "runner_failure": True}
    elapsed = Decimal(str(time.perf_counter() - started))
    try:
        if not result_path.is_file():
            raise StudyError("evaluator exited without a result file")
        result_size = result_path.stat().st_size
        if result_size > max_result_bytes:
            raise StudyError("candidate result exceeds max_candidate_result_bytes")
        request = load_json(request_path)
        result = validate_candidate_result(load_json(result_path), request, int(return_code),
                                           result_size, max_result_bytes)
        # Store a portable canonical form after validating the C++ record. This
        # normalizes serialization only; all measurements and statuses remain
        # byte-for-byte values returned by the evaluator.
        normalized = canonical_json(result) + b"\n"
        if len(normalized) > max_result_bytes:
            raise StudyError("canonical candidate result exceeds max_candidate_result_bytes")
        atomic_write(result_path, normalized)
        host = {"return_code": return_code, "elapsed_wall_seconds": elapsed,
                "stdout": bounded_text(stdout), "stderr": bounded_text(stderr), "runner_failure": False}
        return result, host
    except (StudyError, OSError, TypeError, ValueError) as error:
        try:
            result_path.unlink()
        except FileNotFoundError:
            pass
        message = str(error)
        record = runner_failure_record(candidate_id, build_identity, "failed", "evaluator_crash", message)
        host = {"return_code": return_code, "elapsed_wall_seconds": elapsed,
                "stdout": bounded_text(stdout), "stderr": bounded_text(stderr or message), "runner_failure": True}
        return record, host


def validate_study_trace(path: pathlib.Path, request: dict[str, Any], trace_result: dict[str, Any],
                         maximum_bytes: int) -> tuple[bool, str | None, bytes | None]:
    export = trace_result["trace_export"]
    if export.get("status") != "written":
        return False, export.get("unavailable_reason") or export.get("status"), None
    try:
        raw = path.read_bytes()
    except OSError:
        return False, "trace_file_missing", None
    if len(raw) > maximum_bytes or len(raw) != export.get("bytes"):
        return False, "trace_byte_count_mismatch", None
    if hashlib.sha256(raw).hexdigest() != export.get("sha256"):
        return False, "trace_sha256_mismatch", None
    if not raw.endswith(b"\n"):
        return False, "trace_missing_final_newline", None
    lines = raw.splitlines()
    if not lines:
        return False, "trace_empty", None
    try:
        records = [load_json_line(line) for line in lines]
    except StudyError as error:
        return False, str(error), None
    metadata = records[0]
    metadata_fields = {"schema_id", *REQUEST_IDENTITY_FIELDS, "time_unit", "energy_unit", "activity_assumption"}
    if set(metadata) != metadata_fields or metadata.get("schema_id") != "axissim.study-task-trace.v1":
        return False, "trace_header_schema_mismatch", None
    if any(metadata.get(field) != request.get(field) for field in REQUEST_IDENTITY_FIELDS):
        return False, "trace_identity_mismatch", None
    if metadata.get("time_unit") != "us" or metadata.get("energy_unit") != "J" or metadata.get("activity_assumption") != "uniform_over_task_interval":
        return False, "trace_header_assumption_mismatch", None
    task_fields = {"record_type", "task_sequence", "work_unit_id", "task_name", "resource", "release_us",
                   "ready_us", "start_us", "scheduled_end_us", "observed_end_us", "completion_status", "energy_j"}
    energy_fields = {"vector", "npu_arithmetic", "dram", "dma"}
    for record in records[1:]:
        if set(record) != task_fields or record.get("record_type") != "task":
            return False, "trace_task_schema_mismatch", None
        if record.get("resource") not in ("dma", "rvv", "npu") or record.get("completion_status") not in ("completed", "in_flight_at_censor"):
            return False, "trace_task_enum_mismatch", None
        if not isinstance(record.get("energy_j"), dict) or set(record["energy_j"]) != energy_fields:
            return False, "trace_energy_schema_mismatch", None
    if len(records) - 1 != export.get("record_count"):
        return False, "trace_record_count_mismatch", None
    return True, None, raw


def load_json_line(raw_line: bytes) -> dict[str, Any]:
    try:
        parsed = json.loads(raw_line.decode("utf-8"), parse_int=Decimal, parse_float=Decimal,
                            parse_constant=_reject_constant, object_pairs_hook=_pairs_no_duplicates)
    except (UnicodeError, json.JSONDecodeError, InvalidOperation, StudyError) as error:
        raise StudyError(f"invalid trace JSONL record: {error}") from error
    if not isinstance(parsed, dict):
        raise StudyError("trace JSONL record is not an object")
    canonical_json(parsed)
    return parsed


def same_simulation_result(left: dict[str, Any], right: dict[str, Any]) -> bool:
    runner_fields = {"trace_export", "process_return_code"}
    left_view = {key: value for key, value in left.items() if key not in runner_fields}
    right_view = {key: value for key, value in right.items() if key not in runner_fields}
    return canonical_json(left_view) == canonical_json(right_view)


def _metric_objective(metric_id: str, direction: str, integer: bool = False) -> dict[str, Any]:
    return {"objective_id": metric_id, "direction": direction, "kind": "integer" if integer else "numeric"}


def candidate_objectives(result: dict[str, Any], request: dict[str, Any]) -> tuple[list[dict[str, Any]], dict[str, Any], bool]:
    measurements = result.get("measurements")
    if not isinstance(measurements, dict):
        return [], {}, False
    work_units = measurements.get("work_units", {})
    scheduled = work_units.get("scheduled_target_cohort", {})
    latency = measurements.get("latency", {})
    deadline = measurements.get("deadline", {})
    deadline_policy = request["measurement_protocol"]["deadline_policy"]
    objectives: list[dict[str, Any]] = []
    values: dict[str, Any] = {}
    hard_deadlines_pass = True
    for cohort in ("frame", "control"):
        if int(scheduled.get(cohort, 0)) <= 0:
            continue
        cohort_latency = latency.get(cohort, {})
        for field, label in (("mean_us", "mean_latency_us"), ("p99_us", "p99_latency_us")):
            objective_id = f"{cohort}.{label}"
            objectives.append(_metric_objective(objective_id, "minimize"))
            values[objective_id] = cohort_latency.get(field)
        policy = deadline_policy.get(cohort, {})
        handling = policy.get("handling", "not_applicable")
        deadline_result = deadline.get(cohort, {})
        miss_count = deadline_result.get("miss_count")
        if handling == "objective":
            objective_id = f"{cohort}.deadline_miss_count"
            objectives.append(_metric_objective(objective_id, "minimize", integer=True))
            values[objective_id] = miss_count
        elif handling == "hard_constraint":
            hard_deadlines_pass = hard_deadlines_pass and miss_count == 0
    objectives.extend([
        _metric_objective("energy_per_completed_work_unit_j", "minimize"),
        _metric_objective("throughput_work_units_per_second", "maximize"),
    ])
    values["energy_per_completed_work_unit_j"] = measurements.get("energy", {}).get("energy_per_completed_work_unit_j")
    values["throughput_work_units_per_second"] = measurements.get("throughput_work_units_per_second")
    return objectives, values, hard_deadlines_pass


def result_is_frontier_eligible(result: dict[str, Any], objectives: list[dict[str, Any]],
                                values: dict[str, Any], hard_deadlines_pass: bool) -> tuple[bool, str | None]:
    if result.get("execution_status") != "completed":
        return False, "measurement_not_completed"
    if result.get("power_budget_status") != "pass":
        return False, "modeled_budget_not_passed"
    measurements = result.get("measurements")
    if not isinstance(measurements, dict) or not measurements.get("interval", {}).get("complete", False):
        return False, "measurement_interval_incomplete"
    if measurements.get("work_units", {}).get("incomplete_target_cohort", {}).get("total", 1) != 0:
        return False, "target_cohort_incomplete"
    if result.get("operating_state") == "backlog_growing":
        return False, "backlog_growing"
    if not hard_deadlines_pass:
        return False, "hard_deadline_miss"
    if not objectives or any(values.get(objective["objective_id"]) is None for objective in objectives):
        return False, "objective_unavailable"
    return True, None


def _dominance_epsilon(a: Decimal, b: Decimal) -> Decimal:
    return Decimal("1e-9") * max(Decimal(1), abs(a), abs(b))


def _strictly_better(a: Any, b: Any, objective: dict[str, Any]) -> bool:
    if objective["kind"] == "integer":
        return int(a) < int(b) if objective["direction"] == "minimize" else int(a) > int(b)
    left = Decimal(a)
    right = Decimal(b)
    epsilon = _dominance_epsilon(left, right)
    if objective["direction"] == "minimize":
        return left < right - epsilon
    return left > right + epsilon


def _no_worse(a: Any, b: Any, objective: dict[str, Any]) -> bool:
    if objective["kind"] == "integer":
        return int(a) <= int(b) if objective["direction"] == "minimize" else int(a) >= int(b)
    left = Decimal(a)
    right = Decimal(b)
    epsilon = _dominance_epsilon(left, right)
    if objective["direction"] == "minimize":
        return left <= right + epsilon
    return left >= right - epsilon


def dominates(candidate_a: dict[str, Any], candidate_b: dict[str, Any],
              objectives: list[dict[str, Any]]) -> bool:
    values_a = candidate_a["objective_values"]
    values_b = candidate_b["objective_values"]
    if any(values_a.get(item["objective_id"]) is None or values_b.get(item["objective_id"]) is None
           for item in objectives):
        return False
    all_no_worse = all(_no_worse(values_a[item["objective_id"]], values_b[item["objective_id"]], item)
                       for item in objectives)
    some_strict = any(_strictly_better(values_a[item["objective_id"]], values_b[item["objective_id"]], item)
                      for item in objectives)
    return all_no_worse and some_strict


def pareto_frontier(candidates: list[dict[str, Any]], objectives: list[dict[str, Any]]) -> list[dict[str, Any]]:
    ordered = sorted(candidates, key=lambda candidate: candidate["candidate_id"])
    return [candidate for candidate in ordered
            if not any(other["candidate_id"] != candidate["candidate_id"] and dominates(other, candidate, objectives)
                       for other in ordered)]


def metric_equivalence_classes(candidates: list[dict[str, Any]],
                               objectives: list[dict[str, Any]]) -> list[list[str]]:
    """Return stable candidate groups tied across every applicable objective."""
    classes: list[list[dict[str, Any]]] = []
    ordered = sorted(candidates, key=lambda candidate: candidate["candidate_id"])
    for candidate in ordered:
        values = candidate["objective_values"]
        if any(values.get(item["objective_id"]) is None for item in objectives):
            continue
        destination = None
        for equivalence_class in classes:
            representative = equivalence_class[0]["objective_values"]
            equivalent = True
            for objective in objectives:
                left = values[objective["objective_id"]]
                right = representative[objective["objective_id"]]
                if objective["kind"] == "integer":
                    equivalent = int(left) == int(right)
                else:
                    equivalent = (not _strictly_better(left, right, objective)
                                  and not _strictly_better(right, left, objective))
                if not equivalent:
                    break
            if equivalent:
                destination = equivalence_class
                break
        if destination is None:
            classes.append([candidate])
        else:
            destination.append(candidate)
    return [[candidate["candidate_id"] for candidate in equivalence_class]
            for equivalence_class in classes if len(equivalence_class) > 1]


def coverage_for_group(result: dict[str, Any], swept_factors: list[str]) -> tuple[list[dict[str, Any]] | None,
                                                                                     str | None, str | None]:
    coverage = result.get("power_accounting_coverage")
    if not isinstance(coverage, dict) or coverage.get("schema_id") != "axissim.power-coverage.v1":
        return None, None, None
    factor_records = coverage.get("factors")
    if not isinstance(factor_records, list):
        return None, None, None
    by_id = {record.get("factor_id"): record for record in factor_records if isinstance(record, dict)}
    entries: set[str] = set()
    applicable_cost_factors = 0
    for factor_id in swept_factors:
        record = by_id.get(factor_id)
        if record is None:
            return None, None, None
        applicability = record.get("applicability")
        power_coverage = record.get("power_coverage")
        if applicability == "n/a" or power_coverage == "n/a":
            continue
        if applicability != "modeled" or power_coverage not in ("complete", "partial", "missing"):
            return None, None, None
        applicable_cost_factors += 1
        if power_coverage in ("partial", "missing"):
            omitted = record.get("omitted_terms", [])
            if not omitted:
                return None, None, None
            entries.update(omitted)
    ordered_entries = sorted(entries, key=lambda entry: entry.encode("utf-8"))
    signature = digest(ordered_entries)
    if ordered_entries:
        scope = "exploratory_missing_costs"
    elif applicable_cost_factors:
        scope = "current_model_covered"
    else:
        scope = "not_applicable"
    return ordered_entries, signature, scope


def _group_identity(manifest: dict[str, Any], workload: dict[str, Any], stratum: dict[str, Any],
                    group: dict[str, Any], candidate: dict[str, Any]) -> tuple[str, str, dict[str, Any]]:
    factor_order = manifest["factor_order"]
    factors = [factor for factor in factor_order if factor in group["factors"]]
    interaction_id = f"oat:{factors[0]}" if len(factors) == 1 else group["group_id"]
    swept_factors = [{"factor_id": factor, "levels": manifest["factor_levels"][factor]} for factor in factors]
    candidate_rows = [
        [{"factor_id": factor, "value": row[factor]} for factor in factors]
        for row in group["candidate_rows"]
    ]
    held_values = group.get("held_constant_factor_assignments", {}) or {}
    if isinstance(held_values, list):
        held_values = {entry["factor_id"]: entry["value"] for entry in held_values}
    held_assignments = [{"factor_id": factor, "value": held_values[factor]}
                        for factor in sorted(held_values, key=lambda item: item.encode("utf-8"))]
    request = build_candidate_request(manifest, candidate, manifest["source_hashes"], False)
    sweep_payload = {
        "manifest_id": digest(manifest),
        "workload_identity": candidate["workload_identity"],
        "measurement_protocol_identity": candidate["measurement_protocol_identity"],
        "release_stratum_id": stratum["release_stratum_id"],
        "comparison_class": group["comparison_class"],
        "interaction_id": interaction_id,
        "swept_factors": swept_factors,
        "candidate_rows": candidate_rows,
        "held_constant_factor_assignments": held_assignments,
    }
    sweep_group_id = digest(sweep_payload)
    config_keys = workload.get("factor_config_keys", {})
    swept_config_keys = {config_keys[factor] for factor in factors if factor in config_keys}
    fixed_overrides = {key: value for key, value in request["configuration"]["overrides"].items()
                       if key not in swept_config_keys}
    fixed_payload = {
        "manifest_id": digest(manifest),
        "simulator_build_identity": candidate["simulator_build_identity"],
        "accounting_model_id": manifest["accounting_model_id"],
        "base_config_path": request["configuration"]["base_config_path"],
        "base_config_sha256": request["configuration"]["base_config_sha256"],
        "included_config_hashes": request["configuration"]["included_config_hashes"],
        "power_overlay": request["power_overlay"],
        "fixed_overrides": fixed_overrides,
    }
    return sweep_group_id, digest(fixed_payload), sweep_payload


def build_comparison_report(manifest: dict[str, Any], candidates: list[dict[str, Any]],
                            result_records: dict[str, dict[str, Any]],
                            requests: dict[str, dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], set[str]]:
    workload_by_id = {item["workload_id"]: item for item in manifest["workloads"]}
    stratum_by_id = {workload["workload_id"]: {item["release_stratum_id"]: item for item in workload["release_strata"]}
                     for workload in manifest["workloads"]}
    candidate_by_id = {candidate["candidate_id"]: candidate for candidate in candidates}
    memberships_by_candidate: dict[str, list[dict[str, Any]]] = {candidate["candidate_id"]: [] for candidate in candidates}
    structural_groups: list[dict[str, Any]] = []
    comparison_groups: dict[str, dict[str, Any]] = {}

    for manifest_group in manifest["groups"]:
        for workload_id in manifest_group["workload_ids"]:
            workload = workload_by_id[workload_id]
            for stratum in workload["release_strata"]:
                row_candidates: list[tuple[int, dict[str, Any]]] = []
                for candidate in candidates:
                    if candidate["workload_id"] != workload_id or candidate["release_stratum_id"] != stratum["release_stratum_id"]:
                        continue
                    memberships = [item for item in candidate["group_memberships"] if item["group_id"] == manifest_group["group_id"]]
                    row_candidates.extend((int(item["row_index"]), candidate) for item in memberships)
                row_candidates.sort(key=lambda entry: entry[0])
                if not row_candidates:
                    continue
                structural = {
                    "manifest_group_id": manifest_group["group_id"],
                    "workload_id": workload_id,
                    "family_id": workload["family_id"],
                    "release_stratum_id": stratum["release_stratum_id"],
                    "comparison_class": manifest_group["comparison_class"],
                    "swept_factors": [factor for factor in manifest["factor_order"] if factor in manifest_group["factors"]],
                    "candidate_rows": [],
                    "comparison_group_ids": [],
                }
                for row_index, candidate in row_candidates:
                    result = result_records[candidate["candidate_id"]]
                    request = requests[candidate["candidate_id"]]
                    swept_factors = structural["swept_factors"]
                    coverage_entries, coverage_signature, recommendation_scope = coverage_for_group(result, swept_factors)
                    candidate_scope = (recommendation_scope if result.get("execution_status") == "completed" else None)
                    sweep_group_id, fixed_model_identity, sweep_payload = _group_identity(
                        manifest, workload, stratum, manifest_group, candidate
                    )
                    objectives, objective_values, hard_deadlines_pass = candidate_objectives(result, request)
                    objective_mask = [objective["objective_id"] for objective in objectives]
                    operating_state = result.get("operating_state", "unknown")
                    membership_record = {
                        "manifest_group_id": manifest_group["group_id"],
                        "sweep_group_id": sweep_group_id,
                        "comparison_class": manifest_group["comparison_class"],
                        "row_index": row_index,
                        "swept_values": next(item["swept_row"] for item in candidate["group_memberships"]
                                              if item["group_id"] == manifest_group["group_id"]),
                        "coverage_signature_entries": coverage_entries,
                        "coverage_signature": coverage_signature,
                        "recommendation_scope": candidate_scope,
                        "fixed_model_assumption_identity": fixed_model_identity,
                        "objective_applicability": objective_mask,
                        "operating_state": operating_state,
                    }
                    memberships_by_candidate[candidate["candidate_id"]].append(membership_record)
                    eligible, ineligible_reason = result_is_frontier_eligible(result, objectives, objective_values,
                                                                               hard_deadlines_pass)
                    if recommendation_scope == "not_applicable":
                        eligible, ineligible_reason = False, "swept_factors_not_applicable"
                    row_summary = {
                        "candidate_id": candidate["candidate_id"],
                        "row_index": row_index,
                        "swept_values": membership_record["swept_values"],
                        "execution_status": result.get("execution_status", "failed"),
                        "power_budget_status": result.get("power_budget_status", "not_evaluated"),
                        "operating_state": operating_state,
                        "coverage_signature": coverage_signature,
                        "recommendation_scope": candidate_scope,
                        "objective_values": objective_values,
                        "frontier_eligible": eligible,
                        "frontier_ineligible_reason": ineligible_reason,
                    }
                    structural["candidate_rows"].append(row_summary)

                    if coverage_signature is None or recommendation_scope is None:
                        continue
                    key_payload = {
                        "simulator_build_identity": candidate["simulator_build_identity"],
                        "workload_identity": candidate["workload_identity"],
                        "sweep_group_id": sweep_group_id,
                        "measurement_protocol_identity": candidate["measurement_protocol_identity"],
                        "fixed_model_assumption_identity": fixed_model_identity,
                        "coverage_signature": coverage_signature,
                        "objective_applicability": objective_mask,
                        "operating_state": operating_state,
                    }
                    comparison_id = digest(key_payload)
                    comparison = comparison_groups.setdefault(comparison_id, {
                        "comparison_group_id": comparison_id,
                        "sweep_group_id": sweep_group_id,
                        "manifest_group_id": manifest_group["group_id"],
                        "workload_id": workload_id,
                        "family_id": workload["family_id"],
                        "release_stratum_id": stratum["release_stratum_id"],
                        "comparison_class": manifest_group["comparison_class"],
                        "simulator_build_identity": candidate["simulator_build_identity"],
                        "workload_identity": candidate["workload_identity"],
                        "measurement_protocol_identity": candidate["measurement_protocol_identity"],
                        "fixed_model_assumption_identity": fixed_model_identity,
                        "coverage_signature": coverage_signature,
                        "coverage_signature_entries": coverage_entries,
                        "recommendation_scope": recommendation_scope,
                        "power_coverage_scope": ("swept_factors_only" if recommendation_scope != "not_applicable"
                                                 else "swept_factors_not_applicable"),
                        "frontier_kind": ("exploratory" if recommendation_scope == "exploratory_missing_costs"
                                          else "provisional_covered" if recommendation_scope == "current_model_covered"
                                          else "not_applicable"),
                        "evidence_scope_note": (
                            "Sparse combined-factor sample: the listed SRAM capacities and NPU tile tuples are not fully crossed; M, N, and K vary together. Results describe only these sampled configurations and cannot isolate an independent capacity-by-tile interaction or individual tile-dimension effects."
                            if manifest_group["group_id"] == "interaction:sram_capacity_x_tile_mapping" else
                            "Multi-factor results describe the listed combinations; attribute an effect to one factor only where the rows provide a matched comparison."
                            if manifest_group["group_id"].startswith("interaction:") else None
                        ),
                        "objective_applicability": objectives,
                        "operating_state": operating_state,
                        "candidate_ids": [],
                        "eligible_candidate_ids": [],
                        "frontier_candidate_ids": [],
                        "frontier_status": "not_computed",
                    })
                    comparison["candidate_ids"].append(candidate["candidate_id"])
                    if eligible:
                        comparison["eligible_candidate_ids"].append(candidate["candidate_id"])
                        comparison.setdefault("_eligible_records", []).append({
                            "candidate_id": candidate["candidate_id"],
                            "objective_values": objective_values,
                        })
                    structural["comparison_group_ids"].append(comparison_id)
                structural["comparison_group_ids"] = sorted(set(structural["comparison_group_ids"]))
                structural_groups.append(structural)

    frontier_candidate_ids: set[str] = set()
    comparison_reports = []
    for comparison in sorted(comparison_groups.values(), key=lambda item: item["comparison_group_id"]):
        if comparison["comparison_class"] != "architecture":
            comparison["frontier_status"] = "sensitivity_only"
        elif comparison["recommendation_scope"] == "not_applicable":
            comparison["frontier_status"] = "not_applicable_no_frontier"
        elif comparison["operating_state"] == "backlog_growing":
            comparison["frontier_status"] = "overloaded_no_frontier"
        else:
            points = comparison.pop("_eligible_records", [])
            frontier = pareto_frontier(points, comparison["objective_applicability"])
            comparison["frontier_candidate_ids"] = [point["candidate_id"] for point in frontier]
            comparison["metric_equivalence_classes"] = metric_equivalence_classes(
                frontier, comparison["objective_applicability"])
            frontier_candidate_ids.update(comparison["frontier_candidate_ids"])
            comparison["frontier_status"] = "computed" if points else "no_eligible_candidates"
        comparison.pop("_eligible_records", None)
        comparison["candidate_ids"] = sorted(set(comparison["candidate_ids"]))
        comparison["eligible_candidate_ids"] = sorted(set(comparison["eligible_candidate_ids"]))
        comparison_reports.append(comparison)
    return structural_groups, comparison_reports, frontier_candidate_ids


def baseline_candidate_ids(manifest: dict[str, Any], candidates: list[dict[str, Any]]) -> list[str]:
    selected: list[str] = []
    for workload in manifest["workloads"]:
        stratum_id = workload.get("pilot_release_stratum_id", workload["release_strata"][0]["release_stratum_id"])
        stratum = next(item for item in workload["release_strata"] if item["release_stratum_id"] == stratum_id)
        fixed_overrides = dict(workload.get("fixed_overrides", {}))
        fixed_overrides.update(stratum.get("config_overrides", {}))
        config_keys = workload.get("factor_config_keys", {})
        base_assignments = {factor: value for factor, value in workload["base_factor_assignments"].items()
                            if workload["applicability"].get(factor) == "modeled"}
        if "release_schedule" in manifest["factor_order"]:
            base_assignments["release_schedule"] = stratum_id
        matches: list[dict[str, Any]] = []
        for candidate in candidates:
            if candidate["workload_id"] != workload["workload_id"] or candidate["release_stratum_id"] != stratum_id:
                continue
            assignments = {entry["factor_id"]: entry["value"] for entry in candidate["factor_assignments"]}
            if assignments != base_assignments:
                continue
            overrides = candidate["configuration_overrides"]
            if any(overrides.get(key) != value for key, value in fixed_overrides.items()):
                continue
            if any(key not in fixed_overrides and value != base_assignments.get(factor)
                   for factor, key in config_keys.items() for key, value in overrides.items() if key == config_keys.get(factor)):
                continue
            matches.append(candidate)
        if not matches:
            raise StudyError(f"no selected candidate matches the declared baseline for {workload['workload_id']}/{stratum_id}")
        # The same baseline assignment can occur in several groups with equivalent
        # values but different explicit override envelopes. Candidate order is the
        # manifest order, so use its first baseline anchor deterministically.
        selected.append(matches[0]["candidate_id"])
    return selected


def _candidate_summary_record(candidate: dict[str, Any], result: dict[str, Any], request: dict[str, Any],
                              result_relpath: str, request_relpath: str,
                              memberships: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    status = result.get("execution_status")
    if result.get("schema_id") == RUNNER_FAILURE_SCHEMA:
        status = result["runner_status"]
    measurements = result.get("measurements") if result.get("schema_id") != RUNNER_FAILURE_SCHEMA else None
    runner_failure = None
    if result.get("schema_id") == RUNNER_FAILURE_SCHEMA:
        runner_failure = {key: result[key] for key in (
            "schema_id", "candidate_id", "simulator_build_identity", "runner_status", "failure_reason", "message")}
    return {
        "candidate_id": candidate["candidate_id"],
        "workload_id": candidate["workload_id"],
        "family_id": candidate["family_id"],
        "release_stratum_id": candidate["release_stratum_id"],
        "simulator_build_identity": candidate["simulator_build_identity"],
        "manifest_id": request["manifest_id"],
        "configuration_identity": candidate["configuration_identity"],
        "workload_identity": candidate["workload_identity"],
        "model_assumption_identity": candidate["model_assumption_identity"],
        "measurement_protocol_identity": candidate["measurement_protocol_identity"],
        "execution_status": status,
        "runner_failure": runner_failure,
        "process_return_code": result.get("process_return_code"),
        "power_budget_status": result.get("power_budget_status", "not_evaluated"),
        "configured_it_power_budget_w": result.get("configured_it_power_budget_w"),
        "feasibility_label": result.get("feasibility_label"),
        "operating_state": result.get("operating_state", "unknown"),
        "measurements": measurements,
        "power_accounting_coverage": result.get("power_accounting_coverage"),
        "trace_export": result.get("trace_export"),
        "errors": result.get("errors", []),
        "request_path": request_relpath,
        "result_path": result_relpath,
        "group_memberships": memberships or [],
    }


def _result_status(record: dict[str, Any]) -> str:
    if record.get("schema_id") == RUNNER_FAILURE_SCHEMA:
        return record.get("runner_status", "failed")
    return record.get("execution_status", "failed")


def _retained_bytes(output_root: pathlib.Path) -> int:
    total = 0
    for path in output_root.rglob("*"):
        if not path.is_file() or "host-diagnostics" in path.relative_to(output_root).parts or ".staging" in path.relative_to(output_root).parts:
            continue
        total += path.stat().st_size
    return total


def _write_output_file(output_root: pathlib.Path, path: pathlib.Path, content: bytes,
                       max_output_bytes: int, summary_reserve: int = 0) -> bool:
    current = _retained_bytes(output_root)
    previous = path.stat().st_size if path.is_file() else 0
    if current - previous + len(content) + summary_reserve > max_output_bytes:
        return False
    atomic_write(path, content)
    return True


def _append_host_diagnostic(output_root: pathlib.Path, record: dict[str, Any]) -> None:
    path = output_root / "host-diagnostics" / "execution.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("ab") as stream:
        stream.write(canonical_json(record) + b"\n")


def _candidate_wall_timing_summary(projected_candidate_wall_seconds: Any,
                                   first_pass_hosts: list[dict[str, Any]],
                                   trace_replay_hosts: list[dict[str, Any]]) -> dict[str, Any]:
    """Summarize evaluator subprocess time separately from whole-study wall time."""
    first_pass = sum((Decimal(str(record.get("elapsed_wall_seconds", 0))) for record in first_pass_hosts), Decimal(0))
    trace_replay = sum((Decimal(str(record.get("elapsed_wall_seconds", 0))) for record in trace_replay_hosts), Decimal(0))
    candidate_total = first_pass + trace_replay
    projection = Decimal(str(projected_candidate_wall_seconds))
    return {
        "actual_first_pass_candidate_wall_seconds": first_pass,
        "actual_trace_replay_candidate_wall_seconds": trace_replay,
        "actual_candidate_wall_seconds_total": candidate_total,
        "preflight_projected_candidate_wall_seconds_including_trace_replay": projection,
        "candidate_wall_underprediction_ratio": (candidate_total / projection if projection > 0 else None),
        "candidate_wall_timing_basis": "sum of candidate evaluator subprocess elapsed times; excludes Python orchestration, Pareto/report generation, and artifact hashing",
        "host_wall_seconds_are_not_simulated_metrics": True,
    }


def _planning_diagnostics(root: pathlib.Path, manifest: dict[str, Any], plan_report: dict[str, Any]) -> dict[str, Any]:
    invalid = [candidate for candidate in plan_report.get("candidates", [])
               if candidate.get("configuration_validation_status") != "valid"]
    return {
        "planning_manifest_schema_id": plan_report.get("schema_id"),
        "planning_report_identity": plan_report.get("plan_id"),
        "planning_candidate_rows_before_deduplication": plan_report.get("candidate_count_before_deduplication"),
        "planning_unique_candidates": plan_report.get("candidate_count_unique"),
        "duplicate_rows_removed": (plan_report.get("candidate_count_before_deduplication", 0)
                                    - plan_report.get("candidate_count_unique", 0)),
        "invalid_sram_fit_candidate_count": len(invalid),
        "invalid_candidates": [{
            "candidate_id": candidate.get("candidate_id"),
            "workload_id": candidate.get("workload_id"),
            "configuration_identity": candidate.get("configuration_identity"),
            "group_memberships": candidate.get("group_memberships", []),
            "validation_error": candidate.get("configuration_validation_error"),
        } for candidate in invalid],
        "selected_manifest_candidate_count": manifest.get("limits", {}).get("max_candidates"),
        "invalid_planning_candidates_executed": 0,
    }


def _empty_not_run_result(candidate: dict[str, Any]) -> dict[str, Any]:
    return {"execution_status": "not_run", "power_budget_status": "not_evaluated",
            "operating_state": "unknown", "measurements": None, "power_accounting_coverage": None,
            "errors": [], "candidate_id": candidate["candidate_id"]}


def _candidate_status_counts(records: list[dict[str, Any]]) -> dict[str, int]:
    counts = {key: 0 for key in ("completed", "invalid", "failed", "censored", "not_run")}
    for record in records:
        if record.get("schema_id") == RUNNER_FAILURE_SCHEMA:
            status = record.get("runner_status", "failed")
        else:
            status = record.get("execution_status", "failed")
        if status not in counts:
            status = "not_run" if status.startswith("not_run") else "failed"
        counts[status] += 1
    return counts


def _summary_metrics(result: dict[str, Any]) -> dict[str, Any] | None:
    measurements = result.get("measurements")
    if not isinstance(measurements, dict):
        return None
    return measurements


def _run_candidate_set(root: pathlib.Path, binary: pathlib.Path, output_root: pathlib.Path,
                       manifest: dict[str, Any], candidates: list[dict[str, Any]],
                       selected_ids: list[str], source_hashes: dict[str, str],
                       max_output_bytes: int, max_result_bytes: int, summary_reserve: int,
                       process_runner: Callable[..., subprocess.CompletedProcess[str]] | None = None
                       ) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]], dict[str, dict[str, Any]],
                                  list[str], list[dict[str, Any]]]:
    candidate_by_id = {candidate["candidate_id"]: candidate for candidate in candidates}
    requests = {candidate["candidate_id"]: build_candidate_request(manifest, candidate, source_hashes, False)
                for candidate in candidates}
    request_sizes = {candidate_id: len(canonical_json(request)) for candidate_id, request in requests.items()}
    selected = [candidate_id for candidate_id in selected_ids if candidate_id in candidate_by_id]
    results = {candidate_id: _empty_not_run_result(candidate) for candidate_id, candidate in candidate_by_id.items()}
    executed_requests: dict[str, dict[str, Any]] = {}
    hosts: dict[str, dict[str, Any]] = {}
    not_run: list[str] = []
    host_diags: list[dict[str, Any]] = []
    remaining = list(selected)
    for index, candidate_id in enumerate(selected):
        candidate = candidate_by_id[candidate_id]
        request = requests[candidate_id]
        request_bytes = canonical_json(request)
        request_path = output_root / "requests" / f"{candidate_id}.json"
        result_path = output_root / "results" / f"{candidate_id}.json"
        future_ids = selected[index + 1:]
        required_reserve = (
            len(request_bytes) + max_result_bytes
            + sum(request_sizes[item] + max_result_bytes for item in future_ids)
            + summary_reserve
        )
        current = _retained_bytes(output_root)
        if current + required_reserve > max_output_bytes:
            not_run = selected[index:]
            for remaining_id in not_run:
                results[remaining_id] = {"execution_status": "not_run_output_cap", "power_budget_status": "not_evaluated",
                                         "operating_state": "unknown", "measurements": None,
                                         "power_accounting_coverage": None, "errors": [],
                                         "candidate_id": remaining_id}
            break
        if shutil.disk_usage(output_root).free < required_reserve:
            not_run = selected[index:]
            for remaining_id in not_run:
                results[remaining_id] = {"execution_status": "not_run_disk_guard", "power_budget_status": "not_evaluated",
                                         "operating_state": "unknown", "measurements": None,
                                         "power_accounting_coverage": None, "errors": [],
                                         "candidate_id": remaining_id}
            break
        if file_sha256(binary) != candidate["simulator_build_identity"] or calculate_source_hashes(root, source_paths(manifest)) != source_hashes:
            not_run = selected[index:]
            for remaining_id in not_run:
                results[remaining_id] = {"execution_status": "not_run_provenance_changed", "power_budget_status": "not_evaluated",
                                         "operating_state": "unknown", "measurements": None,
                                         "power_accounting_coverage": None, "errors": [],
                                         "candidate_id": remaining_id}
            break
        if not _write_output_file(output_root, request_path, request_bytes, max_output_bytes, summary_reserve):
            not_run = selected[index:]
            for remaining_id in not_run:
                results[remaining_id] = {"execution_status": "not_run_output_cap", "power_budget_status": "not_evaluated",
                                         "operating_state": "unknown", "measurements": None,
                                         "power_accounting_coverage": None, "errors": [],
                                         "candidate_id": remaining_id}
            break
        executed_requests[candidate_id] = request
        timeout = float(manifest["limits"]["candidate_wall_timeout_seconds"])
        process_result, host = invoke_candidate(root, binary, request_path, result_path, None, timeout,
                                                candidate_id, candidate["simulator_build_identity"],
                                                max_result_bytes, process_runner)
        if process_result.get("schema_id") == RUNNER_FAILURE_SCHEMA:
            # Persist exactly the runner-failure v1 record. The process exit
            # code belongs in host diagnostics and the enclosing study summary.
            result_bytes = canonical_json(process_result) + b"\n"
            if len(result_bytes) > max_result_bytes or not _write_output_file(
                    output_root, result_path, result_bytes, max_output_bytes, summary_reserve):
                raise StudyError("cannot preserve structured runner failure within output limits")
            process_result["process_return_code"] = host.get("return_code")
        else:
            process_result["process_return_code"] = host["return_code"]
            if result_path.stat().st_size > max_result_bytes:
                raise StudyError("validated candidate result exceeds configured result cap")
        results[candidate_id] = process_result
        hosts[candidate_id] = host
        remaining.pop(0)
        diag = {"phase": "first_pass", "candidate_id": candidate_id,
                "return_code": host.get("return_code"), "elapsed_wall_seconds": host["elapsed_wall_seconds"],
                "runner_failure": host.get("runner_failure", False),
                "stdout": host.get("stdout", ""), "stderr": host.get("stderr", "")}
        host_diags.append(diag)
        _append_host_diagnostic(output_root, diag)
    return results, requests, executed_requests, not_run, host_diags


def _trace_replay(root: pathlib.Path, binary: pathlib.Path, output_root: pathlib.Path,
                  candidate: dict[str, Any], base_request: dict[str, Any], first_result: dict[str, Any],
                  manifest: dict[str, Any], max_output_bytes: int, summary_reserve: int,
                  retained_trace_bytes: int,
                  process_runner: Callable[..., subprocess.CompletedProcess[str]] | None = None
                  ) -> tuple[dict[str, Any], int, dict[str, Any]]:
    request = dict(base_request)
    request["trace_policy"] = {"retain_full_trace": True,
                               "max_trace_bytes": manifest["trace_policy"]["max_trace_bytes"]}
    request_bytes = canonical_json(request)
    request_path = output_root / "requests" / f"{candidate['candidate_id']}.trace.json"
    final_trace_path = output_root / "traces" / f"{candidate['candidate_id']}.jsonl"
    staging = output_root / ".staging"
    staging.mkdir(parents=True, exist_ok=True)
    temp_trace = staging / f"{candidate['candidate_id']}.jsonl"
    temp_result = staging / f"{candidate['candidate_id']}.result.json"
    try:
        if file_sha256(binary) != candidate["simulator_build_identity"] or calculate_source_hashes(root, source_paths(manifest)) != manifest["source_hashes"]:
            return {"status": "failed", "reason": "provenance_changed", "trace_path": None}, retained_trace_bytes, {
                "phase": "trace_replay", "candidate_id": candidate["candidate_id"], "return_code": None,
                "elapsed_wall_seconds": Decimal(0), "runner_failure": True, "stderr": "input identity changed"}
        if not _write_output_file(output_root, request_path, request_bytes, max_output_bytes, summary_reserve):
            return {"status": "omitted_limit", "reason": "max_study_output_bytes", "trace_path": None}, retained_trace_bytes, {
                "phase": "trace_replay", "candidate_id": candidate["candidate_id"], "return_code": None,
                "elapsed_wall_seconds": Decimal(0), "runner_failure": False, "stderr": "request output cap"}
        if shutil.disk_usage(output_root).free < int(manifest["trace_policy"]["max_trace_bytes"]) + int(manifest["limits"]["max_candidate_result_bytes"]):
            return {"status": "omitted_limit", "reason": "available_disk_space", "trace_path": None}, retained_trace_bytes, {
                "phase": "trace_replay", "candidate_id": candidate["candidate_id"], "return_code": None,
                "elapsed_wall_seconds": Decimal(0), "runner_failure": False, "stderr": "insufficient free disk for bounded trace scratch"}
        result, host = invoke_candidate(root, binary, request_path, temp_result, temp_trace,
                                       float(manifest["limits"]["candidate_wall_timeout_seconds"]),
                                       candidate["candidate_id"], candidate["simulator_build_identity"],
                                       int(manifest["limits"]["max_candidate_result_bytes"]), process_runner)
        diag = {"phase": "trace_replay", "candidate_id": candidate["candidate_id"],
                "return_code": host.get("return_code"), "elapsed_wall_seconds": host["elapsed_wall_seconds"],
                "runner_failure": host.get("runner_failure", False), "stdout": host.get("stdout", ""),
                "stderr": host.get("stderr", "")}
        if result.get("schema_id") == RUNNER_FAILURE_SCHEMA:
            return {"status": "failed", "reason": result["failure_reason"], "trace_path": None}, retained_trace_bytes, diag
        if not same_simulation_result(first_result, result):
            try:
                temp_trace.unlink()
            except FileNotFoundError:
                pass
            return {"status": "failed", "reason": "replay_measurement_mismatch", "trace_path": None}, retained_trace_bytes, diag
        valid, reason, raw = validate_study_trace(temp_trace, request, result,
                                                  int(manifest["trace_policy"]["max_trace_bytes"]))
        if not valid or raw is None:
            return {"status": result.get("trace_export", {}).get("status", "failed"),
                    "reason": reason, "trace_path": None}, retained_trace_bytes, diag
        max_retained = int(manifest["limits"]["max_retained_trace_bytes"])
        if retained_trace_bytes + len(raw) > max_retained:
            return {"status": "omitted_limit", "reason": "max_retained_trace_bytes", "trace_path": None}, retained_trace_bytes, diag
        if not _write_output_file(output_root, final_trace_path, raw, max_output_bytes, summary_reserve):
            return {"status": "omitted_limit", "reason": "max_study_output_bytes", "trace_path": None}, retained_trace_bytes, diag
        return {"status": "written", "reason": None,
                "trace_path": final_trace_path.relative_to(output_root).as_posix(),
                "bytes": len(raw), "record_count": result["trace_export"]["record_count"],
                "sha256": result["trace_export"]["sha256"]}, retained_trace_bytes + len(raw), diag
    finally:
        for path in (temp_trace, temp_result):
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def evaluator_outcome_is_accepted(return_code: int, structured_status: str) -> bool:
    """Both process and structured status must agree before a result is accepted."""
    return return_code == 0 and structured_status == "completed"


def _verify_execution_inputs(root: pathlib.Path, manifest_path: pathlib.Path, binary_path: pathlib.Path,
                             plan_report_path: pathlib.Path, preflight_path: pathlib.Path
                             ) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], list[dict[str, Any]], dict[str, str]]:
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict):
        raise StudyError("study manifest root is not an object")
    _validate_top_level(manifest, final=True)
    binary = binary_path.resolve()
    if not binary.is_file():
        raise StudyError("simulator binary does not exist")
    build_identity = file_sha256(binary)
    source_hashes = calculate_source_hashes(root, source_paths(manifest))
    if source_hashes != manifest["source_hashes"]:
        changed = sorted(path for path in set(source_hashes) | set(manifest["source_hashes"])
                         if source_hashes.get(path) != manifest["source_hashes"].get(path))
        raise StudyError("study input hashes changed: " + ", ".join(changed))
    if plan_report_path.resolve() != repo_path(root, manifest["planning_report_path"]).resolve():
        raise StudyError("planning report path differs from manifest planning_report_path")
    planning = load_json(plan_report_path)
    if planning.get("schema_id") != "axissim.study-plan-report.v1":
        raise StudyError("planning report schema is unsupported")
    if planning.get("simulator_build_identity") != build_identity or planning.get("source_hashes") != source_hashes:
        raise StudyError("planning report provenance differs from current executable or inputs")
    if planning.get("status") not in ("planned", "planned_with_invalid_rows"):
        raise StudyError("planning report is not complete")
    preflight = load_json(preflight_path)
    if preflight.get("schema_id") != "axissim.study-preflight-report.v1" or preflight.get("status") != "ready" or preflight.get("structured_status") != "ready":
        raise StudyError("preflight report is not ready")
    if preflight.get("manifest_identity") != digest(manifest):
        raise StudyError("preflight report manifest identity differs")
    if preflight.get("simulator_build_identity") != build_identity:
        raise StudyError("preflight report simulator identity differs")
    if preflight.get("planning_report_identity") != planning.get("plan_id"):
        raise StudyError("preflight report planning identity differs")
    if preflight.get("candidate_simulations_launched") != 0 or preflight.get("study_evaluate_invocations") != 0:
        raise StudyError("preflight report indicates candidate execution")
    if preflight.get("exit_code") != 0 or any(not item.get("pass") for item in preflight.get("cap_checks", {}).values()):
        raise StudyError("preflight report contains a failed operational cap")
    if manifest["limits"]["max_candidates"] != 168 or preflight.get("candidate_count") != 168:
        raise StudyError("M5 requires the approved, unchanged 168-candidate selection")
    expected_workloads = {"physical_ai_baseline": 58, "mnist12_profiled": 56, "gemm_heavy_512": 54}
    if preflight.get("per_workload_candidate_counts") != expected_workloads:
        raise StudyError("M5 workload candidate counts differ from the approved selection")
    if planning.get("candidate_count_before_deduplication") != 398 or planning.get("candidate_count_unique") != 372:
        raise StudyError("planning pool no longer matches the reviewed 398-row / 372-unique study")
    invalid_planning = [item for item in planning.get("candidates", [])
                        if item.get("configuration_validation_status") != "valid"]
    invalid_groups = {member.get("group_id") for item in invalid_planning
                      for member in item.get("group_memberships", [])}
    if len(invalid_planning) != 18 or invalid_groups != {
            "interaction:sram_capacity_x_tile_mapping", "oat:weight_sram_capacity_bytes"}:
        raise StudyError("planning diagnostics no longer contain the reviewed 18 SRAM-fit-invalid configurations")
    candidates, _ = enumerate_candidates(manifest, build_identity, source_hashes)
    planned = preflight.get("candidates")
    if not isinstance(planned, list) or [item.get("candidate_id") for item in planned] != [item["candidate_id"] for item in candidates]:
        raise StudyError("preflight candidate order or identities differ from deterministic manifest expansion")
    if any(item.get("configuration_validation_status") != "valid" for item in planned):
        raise StudyError("preflight contains an invalid selected candidate")
    return manifest, planning, preflight, candidates, source_hashes


def _result_metrics(result: dict[str, Any]) -> dict[str, Any] | None:
    measurements = result.get("measurements")
    if not isinstance(measurements, dict):
        return None
    frame = measurements.get("latency", {}).get("frame", {})
    control = measurements.get("latency", {}).get("control", {})
    frame_deadline = measurements.get("deadline", {}).get("frame", {})
    control_deadline = measurements.get("deadline", {}).get("control", {})
    energy = measurements.get("energy", {})
    return {
        "interval": measurements.get("interval"),
        "work_units": measurements.get("work_units"),
        "throughput_work_units_per_second": measurements.get("throughput_work_units_per_second"),
        "frame_mean_latency_us": frame.get("mean_us"),
        "frame_p99_latency_us": frame.get("p99_us"),
        "frame_deadline_miss_count": frame_deadline.get("miss_count"),
        "frame_deadline_miss_rate": frame_deadline.get("miss_rate"),
        "control_mean_latency_us": control.get("mean_us"),
        "control_p99_latency_us": control.get("p99_us"),
        "control_deadline_miss_count": control_deadline.get("miss_count"),
        "control_deadline_miss_rate": control_deadline.get("miss_rate"),
        "energy_j": energy.get("total_it_j"),
        "energy_per_completed_work_unit_j": energy.get("energy_per_completed_work_unit_j"),
        "energy_work_unit_denominator": energy.get("energy_work_unit_denominator"),
        "dynamic_energy_by_category_j": energy.get("dynamic_by_category_j"),
        "static_energy_j": energy.get("static_j"),
        "average_modeled_it_power_w": energy.get("average_it_power_w"),
        "backlog": measurements.get("backlog"),
        "last_model_time_us": measurements.get("last_model_time_us"),
    }


def bottleneck_diagnostics(observations: list[dict[str, Any]], groups: list[dict[str, Any]]) -> dict[str, Any]:
    """Summarize observed latency sensitivity without claiming physical attribution."""
    single_factor = []
    for observation in observations:
        if observation.get("comparison_class") != "architecture" or len(observation.get("tested_factors", [])) != 1:
            continue
        low = observation.get("min_frame_mean_latency_us")
        high = observation.get("max_frame_mean_latency_us")
        if low is None or high is None:
            continue
        span = Decimal(high) - Decimal(low)
        single_factor.append({**observation, "latency_span_us": span})
    single_factor.sort(key=lambda row: (-Decimal(row["latency_span_us"]),
                                        row["manifest_group_id"], row["release_stratum_id"]))
    largest_single_factor = single_factor[0] if single_factor else None

    compute_memory = []
    for group in groups:
        if group.get("manifest_group_id") != "interaction:npu_macs_x_dram_bandwidth":
            continue
        cells: dict[tuple[Decimal, Decimal], Decimal] = {}
        for row in group.get("candidate_rows", []):
            swept = row.get("swept_values", {})
            metrics = row.get("metrics") or {}
            macs = swept.get("npu_macs")
            bandwidth = swept.get("dram_bandwidth_gbps")
            latency = metrics.get("frame_mean_latency_us")
            if macs is None or bandwidth is None or latency is None:
                continue
            cells[(Decimal(macs), Decimal(bandwidth))] = Decimal(latency)
        mac_levels = sorted({key[0] for key in cells})
        bandwidth_levels = sorted({key[1] for key in cells})
        if len(mac_levels) < 2 or len(bandwidth_levels) < 2:
            continue
        bandwidth_spans = []
        for macs in mac_levels:
            values = [cells[(macs, bandwidth)] for bandwidth in bandwidth_levels
                      if (macs, bandwidth) in cells]
            if len(values) >= 2:
                bandwidth_spans.append({"npu_macs": macs, "latency_span_us": max(values) - min(values)})
        compute_spans = []
        for bandwidth in bandwidth_levels:
            values = [cells[(macs, bandwidth)] for macs in mac_levels if (macs, bandwidth) in cells]
            if len(values) >= 2:
                compute_spans.append({"dram_bandwidth_gbps": bandwidth,
                                      "latency_span_us": max(values) - min(values)})
        if not bandwidth_spans or not compute_spans:
            continue
        max_bandwidth_span = max(row["latency_span_us"] for row in bandwidth_spans)
        max_compute_span = max(row["latency_span_us"] for row in compute_spans)
        complete_grid = len(cells) == len(mac_levels) * len(bandwidth_levels)
        scope_values = {row.get("recommendation_scope") for row in group.get("candidate_rows", [])
                        if row.get("recommendation_scope") is not None}
        recommendation_scope = (next(iter(scope_values)) if len(scope_values) == 1
                                else "mixed" if scope_values else None)
        if max_bandwidth_span > max_compute_span:
            interpretation = ("Within the sampled grid, latency varies more across DRAM bandwidth levels than across NPU MAC counts. "
                              "This is evidence of bandwidth sensitivity in this model, not a calibrated physical bottleneck.")
        else:
            interpretation = ("Within the sampled grid, latency varies at least as much across NPU MAC counts as across DRAM bandwidth levels. "
                              "This is a model sensitivity result, not a calibrated physical bottleneck.")
        if not complete_grid:
            interpretation += " The sampled grid is incomplete, so no full interaction or migration claim is made."
        else:
            interpretation += " The tested grid does not by itself establish a physical bottleneck migration."
        compute_memory.append({
            "manifest_group_id": group["manifest_group_id"],
            "release_stratum_id": group["release_stratum_id"],
            "recommendation_scope": recommendation_scope,
            "power_coverage_scope": ("swept_factors_not_applicable" if recommendation_scope == "not_applicable"
                                     else "swept_factors_only" if recommendation_scope else None),
            "npu_macs_levels": mac_levels,
            "dram_bandwidth_gbps_levels": bandwidth_levels,
            "complete_factor_grid": complete_grid,
            "bandwidth_latency_spans_by_npu_macs": bandwidth_spans,
            "mac_latency_spans_by_dram_bandwidth": compute_spans,
            "maximum_bandwidth_latency_span_us": max_bandwidth_span,
            "maximum_mac_latency_span_us": max_compute_span,
            "interpretation": interpretation,
        })
    compute_memory.sort(key=lambda row: (row["manifest_group_id"], row["release_stratum_id"]))
    return {
        "largest_single_factor_latency_sensitivity": largest_single_factor,
        "compute_memory_interactions": compute_memory,
        "interpretation_limit": "These are ranges over tested AxisSim rows, not event-level stall attribution, calibrated silicon bottlenecks, or statistical uncertainty intervals.",
    }


def _csv_bytes(group: dict[str, Any], requests: dict[str, dict[str, Any]],
               results: dict[str, dict[str, Any]]) -> bytes:
    from io import StringIO
    columns = [item["objective_id"] for item in group["objective_applicability"]]
    buffer = StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(["candidate_id", "power_budget_status", "recommendation_scope", *columns])
    for candidate_id in group["frontier_candidate_ids"]:
        values = candidate_objectives(results[candidate_id], requests[candidate_id])[1]
        writer.writerow([candidate_id, results[candidate_id].get("power_budget_status", "not_evaluated"),
                         group["recommendation_scope"], *[values.get(column) for column in columns]])
    return buffer.getvalue().encode("utf-8")


def _canonical_artifact_inventory(output_root: pathlib.Path) -> list[dict[str, Any]]:
    rows = []
    for path in sorted(output_root.rglob("*"), key=lambda item: item.relative_to(output_root).as_posix().encode("utf-8")):
        relative = path.relative_to(output_root)
        if not path.is_file() or "host-diagnostics" in relative.parts or ".staging" in relative.parts or relative.as_posix() == "study-summary.json":
            continue
        raw = path.read_bytes()
        rows.append({"path": relative.as_posix(), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
    return rows


def _workload_report(workload: dict[str, Any], candidates: list[dict[str, Any]],
                     results: dict[str, dict[str, Any]], baseline_id: str | None,
                     structural_groups: list[dict[str, Any]], comparisons: list[dict[str, Any]],
                     planning_diagnostics: dict[str, Any], pilot: bool) -> tuple[dict[str, Any], str]:
    workload_id = workload["workload_id"]
    selected = [item for item in candidates if item["workload_id"] == workload_id]
    outcomes = _candidate_status_counts([results[item["candidate_id"]] for item in selected])
    budget_outcomes = {status: sum(results[item["candidate_id"]].get("power_budget_status") == status
                                   for item in selected)
                       for status in ("pass", "fail", "not_evaluated")}
    baseline_candidate = next((item for item in selected if item["candidate_id"] == baseline_id), None)
    baseline = results.get(baseline_id) if baseline_id else None
    baseline_metrics = _result_metrics(baseline) if baseline else None
    energy_parts = (baseline_metrics or {}).get("dynamic_energy_by_category_j") or {}
    largest_energy = max(((name, value) for name, value in energy_parts.items() if value is not None),
                         key=lambda row: row[1], default=None)
    groups = [group for group in structural_groups if group["workload_id"] == workload_id]
    pareto_groups = [group for group in comparisons if group["workload_id"] == workload_id]
    structural_by_key = {(group["manifest_group_id"], group["workload_id"], group["release_stratum_id"]): group
                         for group in groups}
    architecture_findings = []
    for group in pareto_groups:
        structural = structural_by_key.get((group["manifest_group_id"], workload_id,
                                            group["release_stratum_id"]))
        candidate_rows_by_id = {}
        if structural:
            for row in structural["candidate_rows"]:
                candidate_rows_by_id.setdefault(row["candidate_id"], row)
        frontier_details = []
        for candidate_id in group["frontier_candidate_ids"]:
            row = candidate_rows_by_id.get(candidate_id)
            if row is None:
                continue
            metrics = row.get("metrics") or {}
            frontier_details.append({
                "candidate_id": candidate_id,
                "swept_values": row["swept_values"],
                "objective_values": row["objective_values"],
                "modeled_budget_status": row["power_budget_status"],
                "recommendation_scope": row["recommendation_scope"],
                "mean_latency_us": metrics.get("frame_mean_latency_us"),
                "p99_latency_us": metrics.get("frame_p99_latency_us"),
                "deadline_miss_count": metrics.get("frame_deadline_miss_count"),
                "throughput_work_units_per_second": metrics.get("throughput_work_units_per_second"),
                "energy_per_completed_work_unit_j": metrics.get("energy_per_completed_work_unit_j"),
                "average_modeled_it_power_w": metrics.get("average_modeled_it_power_w"),
            })
        if group["comparison_class"] != "architecture":
            finding_type = "model_sensitivity_only"
            conclusion = "This sweep changes model assumptions and is not an architectural Pareto recommendation."
        elif group["frontier_status"] == "not_applicable_no_frontier":
            finding_type = "not_applicable"
            conclusion = "C++ marks the swept resource as not applicable on this execution path; no improvement claim is made."
        elif group["frontier_candidate_ids"]:
            finding_type = ("exploratory_frontier" if group["frontier_kind"] == "exploratory"
                            else "provisional_covered_frontier")
            conclusion = ("Non-dominated within this model group; incomplete incremental power costs make this exploratory."
                          if finding_type == "exploratory_frontier" else
                          "Non-dominated within this model group; incremental costs for the swept factors are represented, while total system power coverage and feasibility remain provisional.")
        else:
            finding_type = "no_eligible_frontier"
            conclusion = "No candidate met all modeled budget, completion, backlog, and objective eligibility conditions."
        architecture_findings.append({"comparison_group_id": group["comparison_group_id"],
                                      "manifest_group_id": group["manifest_group_id"],
                                      "release_stratum_id": group["release_stratum_id"],
                                      "comparison_class": group["comparison_class"],
                                      "frontier_status": group["frontier_status"],
                                      "frontier_kind": group["frontier_kind"],
                                      "power_coverage_scope": group["power_coverage_scope"],
                                      "swept_factors": structural["swept_factors"] if structural else [],
                                      "evidence_scope_note": group["evidence_scope_note"],
                                      "finding_type": finding_type, "conclusion": conclusion,
                                      "candidate_ids": group["frontier_candidate_ids"],
                                      "frontier_candidate_details": frontier_details,
                                      "metric_equivalence_classes": group.get("metric_equivalence_classes", [])})
    observations = []
    for group in groups:
        completed = [row for row in group["candidate_rows"] if row["execution_status"] == "completed"
                     and row.get("metrics") is not None]
        if not completed:
            continue
        latencies = [(row["candidate_id"], row["metrics"].get("frame_mean_latency_us"))
                     for row in completed if row["metrics"].get("frame_mean_latency_us") is not None]
        if latencies:
            low = min(latencies, key=lambda item: item[1])
            high = max(latencies, key=lambda item: item[1])
            scopes = {row.get("recommendation_scope") for row in completed
                      if row.get("recommendation_scope") is not None}
            recommendation_scope = (next(iter(scopes)) if len(scopes) == 1
                                    else "mixed" if scopes else None)
            observations.append({
                "manifest_group_id": group["manifest_group_id"],
                "comparison_class": group["comparison_class"],
                "release_stratum_id": group["release_stratum_id"],
                "tested_factors": group["swept_factors"],
                "completed_rows": len(completed),
                "min_frame_mean_latency_candidate_id": low[0],
                "min_frame_mean_latency_us": low[1],
                "max_frame_mean_latency_candidate_id": high[0],
                "max_frame_mean_latency_us": high[1],
                "recommendation_scope": recommendation_scope,
                "power_coverage_scope": ("swept_factors_not_applicable" if recommendation_scope == "not_applicable"
                                         else "swept_factors_only" if recommendation_scope else None),
                "interpretation": "Observed AxisSim sensitivity across this group's declared rows and fixed assumptions; not a physical critical-path attribution.",
            })
    report = {
        "schema_id": "axissim.study-workload-report.v1",
        "workload_id": workload_id,
        "family_id": workload["family_id"],
        "release_strata": [item["release_stratum_id"] for item in workload["release_strata"]],
        "measurement_protocol": workload["measurement_protocol"],
        "pilot_only": pilot,
        "selected_candidate_count": len(selected),
        "candidate_outcomes": outcomes,
        "modeled_budget_outcomes": budget_outcomes,
        "baseline_candidate_id": baseline_id,
        "baseline_assumptions": ({
            "configuration_identity": baseline_candidate["configuration_identity"],
            "model_assumption_identity": baseline_candidate["model_assumption_identity"],
            "measurement_protocol_identity": baseline_candidate["measurement_protocol_identity"],
            "release_stratum_id": baseline_candidate["release_stratum_id"],
            "factor_assignments": baseline_candidate["factor_assignments"],
            "configuration_overrides": baseline_candidate["configuration_overrides"],
        } if baseline_candidate else None),
        "baseline_metrics": baseline_metrics,
        "energy_metric_scope": {
            "numerator": "C++ total modeled IT energy accumulated over the measurement interval",
            "denominator": "Completed work units reported as energy_work_unit_denominator (frames plus control units when present)",
            "finite_protocol": "Total IT energy over the finite makespan divided by all completed frame and control work units after drain",
            "sustained_protocol": "IT energy accumulated in [t0,t1) divided by work units completed in [t0,t1); the release cohort used for latency and deadline metrics is reported separately",
            "baseline_denominator_count": (baseline_metrics.get("energy_work_unit_denominator") if baseline_metrics else None),
            "selected_workload_scope": "The selected finite Physical AI, MNIST-12, and GEMM-heavy baseline profiles complete frame work only; their baseline denominator is 30 frames, so J/completed work unit has a per-frame denominator for those records.",
            "comparison_caveat": "Matching frame denominators do not make energy values interchangeable: compare study and GUI/legacy results only when workload inputs, model assumptions, power overlay/configuration, and measurement interval also match.",
        },
        "baseline_power_budget_status": baseline.get("power_budget_status") if baseline else None,
        "largest_modeled_dynamic_energy_category": ({"category": largest_energy[0], "energy_j": largest_energy[1]}
                                                      if largest_energy else None),
        "tested_latency_sensitivity": observations,
        "bottleneck_diagnostics": bottleneck_diagnostics(observations, groups),
        "sensitivity_groups": groups,
        "pareto_groups": pareto_groups,
        "architecture_findings": architecture_findings,
        "planning_diagnostics": planning_diagnostics,
        "limitations": [
            "Results are deterministic AxisSim model outputs under the declared assumptions, not chip measurements.",
            "The 0.23 W budget is average modeled IT power; a pass is only provisionally feasible under current modeled power terms.",
        "DRAM-interface bandwidth scaling and SRAM access/resident-weight costs are incomplete or unpriced; groups changing these costs are exploratory.",
            "Power-cost coverage classifications cover the incremental costs of swept factors only, not complete system power accounting.",
            "Host Arm64/x86 measurements are validation evidence and are not target-system predictions.",
            "The largest tested sensitivity and energy-ledger component are proxies, not a calibrated physical bottleneck attribution.",
        ],
        "evidence_scope": "C++-returned metrics, grouped and summarized without Python-side simulation or power equations.",
    }
    lines = [f"# {workload_id}", "", f"Workload family: `{workload['family_id']}`", "",
             f"Selected candidates: {len(selected)}. Completed: {outcomes['completed']}; invalid: {outcomes['invalid']}; failed: {outcomes['failed']}; censored: {outcomes['censored']}; not run: {outcomes['not_run']}.", ""]
    if pilot:
        lines += ["This is the bounded three-workload pilot. It does not contain the full study or Pareto analysis.", ""]
    lines += ["## Baseline", ""]
    if baseline and baseline.get("execution_status") == "completed" and baseline_metrics:
        lines += [f"Candidate `{baseline_id}`; configuration `{baseline_candidate['configuration_identity']}`; model `{baseline_candidate['model_assumption_identity']}`; budget status `{baseline.get('power_budget_status')}`.", "",
                  "| Mean latency (µs) | P99 (µs) | Deadline misses | Throughput (work units/s) | Energy / completed work unit (J) | Average modeled IT power (W) |", "|---:|---:|---:|---:|---:|---:|"]
        lines.append("| " + " | ".join(str(baseline_metrics.get(key)) for key in (
            "frame_mean_latency_us", "frame_p99_latency_us", "frame_deadline_miss_count", "throughput_work_units_per_second",
            "energy_per_completed_work_unit_j", "average_modeled_it_power_w")) + " |")
        lines += ["", "### Energy metric scope", "",
                  "The C++ energy-per-completed-work-unit value is total modeled IT energy over the measurement interval divided by the reported completed-work-unit denominator (frames plus control units, when present). For a finite run, the interval is the makespan and the denominator includes completed units after drain. For a sustained run, the numerator is energy integrated over `[t0,t1)` and the denominator is units completed inside that same window; latency and deadline metrics use the separately reported release cohort.", "",
                  f"This baseline is a finite frame-only workload: its denominator is {baseline_metrics.get('energy_work_unit_denominator')} completed frames, so its denominator is a frame count. The desktop GUI and legacy CLI show their separate whole-run `IT energy / frame` metric: total IT energy over the makespan divided by completed vision frames. The denominator matches for these frame-only records, but the energy values are not necessarily interchangeable: the study uses its declared power-budget configuration/overlay, while the GUI uses the selected scenario's configuration. Control-task energy can also contribute to a whole-run numerator while the legacy denominator remains vision frames. Compare numeric values only when the workload, model and power assumptions, and measurement interval match."]
        if largest_energy:
            lines += ["", f"Largest dynamic-energy ledger category: `{largest_energy[0]}` ({largest_energy[1]} J). This is a modeled ledger component, not measured chip power."]
    else:
        lines += ["The baseline did not complete; no baseline metrics are claimed."]
    lines += ["", "## Bottleneck evidence from tested model sensitivity", "",
              "These are ranges over tested AxisSim rows, not event-level stall attribution, calibrated silicon bottlenecks, or statistical uncertainty intervals.", ""]
    bottlenecks = report["bottleneck_diagnostics"]
    if bottlenecks["largest_single_factor_latency_sensitivity"]:
        signal = bottlenecks["largest_single_factor_latency_sensitivity"]
        lines.append(f"Largest one-factor architecture-sweep mean-latency range: `{signal['manifest_group_id']}` / `"
                     f"{signal['release_stratum_id']}` varied by {signal['latency_span_us']} µs across its listed settings. "
                     f"Swept-factor power scope: `{signal['recommendation_scope']}`. "
                     "This is the largest range among one-factor architecture sweeps, not across all interactions, and does not claim a physical bottleneck.")
    else:
        lines.append("No completed single-factor architecture sensitivity is available for a bottleneck signal.")
    for interaction in bottlenecks["compute_memory_interactions"]:
        lines.append(f"- `{interaction['manifest_group_id']}` / `{interaction['release_stratum_id']}`: maximum latency span across bandwidth levels at fixed MAC count was {interaction['maximum_bandwidth_latency_span_us']} µs; maximum span across MAC counts at fixed bandwidth was {interaction['maximum_mac_latency_span_us']} µs. {interaction['interpretation']}")
    lines += ["", "## Sensitivity and interaction results", ""]
    if not groups:
        lines += ["No candidate-group results are available in this pilot.", ""]
    for group in groups:
        lines += [f"### `{group['manifest_group_id']}` — {group['comparison_class']} — `{group['release_stratum_id']}`", "",
                  f"Tested factors: {', '.join(group['swept_factors'])}.", "",
                  "| Candidate | Swept values | Status | Modeled budget status | Mean latency (µs) | P99 (µs) | Misses | Throughput (work units/s) | Energy / completed work unit (J) | Average modeled IT power (W) |", "|---|---|---|---|---:|---:|---:|---:|---:|---:|"]
        for row in group["candidate_rows"]:
            metrics = row.get("metrics") or {}
            swept = ", ".join(f"{key}={value}" for key, value in row["swept_values"].items())
            lines.append("| " + " | ".join([row["candidate_id"], swept, row["execution_status"], row["power_budget_status"],
                *[str(metrics.get(key)) for key in ("frame_mean_latency_us", "frame_p99_latency_us",
                                                     "frame_deadline_miss_count", "throughput_work_units_per_second",
                                                     "energy_per_completed_work_unit_j", "average_modeled_it_power_w")]]) + " |")
        lines.append("")
    lines += ["## Pareto groups", ""]
    if pareto_groups:
        lines += ["| Comparison group | Comparison class | Release | Frontier status | Frontier class | Power coverage scope | Service/backlog state | Eligible | Frontier |", "|---|---|---|---|---|---|---|---:|---:|"]
        for group in pareto_groups:
            lines.append(f"| {group['comparison_group_id']} | {group['comparison_class']} | {group['release_stratum_id']} | {group['frontier_status']} | {group['frontier_kind']} | {group['power_coverage_scope']} | {group['operating_state']} | {len(group['eligible_candidate_ids'])} | {len(group['frontier_candidate_ids'])} |")
        lines.append("")
    else:
        lines += ["No Pareto groups were computed.", ""]
    lines += ["## Model-supported conclusions", ""]
    if architecture_findings:
        for finding in architecture_findings:
            group_ref = f"`{finding['manifest_group_id']}` / `{finding['release_stratum_id']}`"
            if finding["frontier_candidate_details"]:
                candidates = []
                for detail in finding["frontier_candidate_details"]:
                    swept = ", ".join(f"{key}={value}" for key, value in detail["swept_values"].items())
                    metrics = detail["objective_values"]
                    metric_text = ", ".join(f"{key}={value}" for key, value in metrics.items())
                    candidates.append(f"`{swept}` (candidate `{detail['candidate_id']}`, {metric_text})")
                candidate_text = " Frontier rows: " + "; ".join(candidates) + "."
            else:
                candidate_text = ""
            equivalence_text = (" Metric-equivalent frontier rows (within the stated dominance tolerance): "
                                + "; ".join(", ".join(f"`{candidate_id}`" for candidate_id in class_ids)
                                             for class_ids in finding["metric_equivalence_classes"]) + "."
                                if finding["metric_equivalence_classes"] else "")
            scope_text = (" " + finding["evidence_scope_note"] if finding["evidence_scope_note"] else "")
            lines.append(f"- {finding['conclusion']} Group: {group_ref}." + candidate_text + equivalence_text + scope_text)
        lines.append("")
    else:
        lines += ["No architecture recommendation is supported by this run.", ""]
    lines += ["## Interpretation and limits", "",
              "A Pareto comparison is local to its workload, release stratum, model assumptions, objective mask, and power-coverage signature. Service/backlog state is included when applicable; these finite makespan workloads have no observed steady-state backlog and show that state as not applicable. No weighted score or cross-workload ranking is used. All candidates tied across every applicable objective within the configured tolerance remain non-dominated; such rows are model-equivalent under the reported objectives, not separate demonstrated improvements.", "",
              "Power-coverage classifications refer to incremental cost coverage of the factors swept in that comparison group. They do not imply that every component of total system power is characterized.", "",
              "The 0.23 W result does not establish physical silicon feasibility. SRAM access/resident-weight costs and DRAM-interface bandwidth scaling are incomplete or unpriced. Arm64/x86 host timing is not used as a target prediction. Sensitivity results do not prove a calibrated hardware bottleneck.", ""]
    return report, "\n".join(lines)


def execute_study(root: pathlib.Path, manifest_path: pathlib.Path, binary_path: pathlib.Path,
                  plan_report_path: pathlib.Path, preflight_path: pathlib.Path,
                  output_root: pathlib.Path, pilot: bool = False) -> tuple[dict[str, Any], int]:
    manifest, planning, preflight, candidates, source_hashes = _verify_execution_inputs(
        root, manifest_path, binary_path, plan_report_path, preflight_path)
    output_root = output_root.resolve()
    if output_root.exists() and any(output_root.iterdir()):
        raise StudyError("output directory must be absent or empty; study artifacts are never overwritten")
    output_root.mkdir(parents=True, exist_ok=True)
    output_cap = int(manifest["limits"]["max_study_output_bytes"])
    result_cap = int(manifest["limits"]["max_candidate_result_bytes"])
    summary_reserve = int(preflight["estimate"]["summary_reserve_bytes"])
    estimated_output = int(preflight["estimate"]["total_output_bytes_worst_case"])
    if estimated_output > output_cap or shutil.disk_usage(output_root).free < estimated_output:
        raise StudyError("preflight storage reserve exceeds the declared output cap or available disk space")

    baseline_ids = baseline_candidate_ids(manifest, candidates)
    selected_ids = baseline_ids if pilot else [item["candidate_id"] for item in candidates]
    planning_diagnostics = _planning_diagnostics(root, manifest, planning)
    wall_started = time.perf_counter()
    results, requests, _, not_run, first_pass_hosts = _run_candidate_set(
        root, binary_path.resolve(), output_root, manifest, candidates, selected_ids, source_hashes,
        output_cap, result_cap, summary_reserve)
    if pilot:
        selected_set = set(selected_ids)
        for candidate in candidates:
            if candidate["candidate_id"] not in selected_set:
                results[candidate["candidate_id"]]["execution_status"] = "not_run_pilot"

    structural_groups: list[dict[str, Any]] = []
    comparisons: list[dict[str, Any]] = []
    frontier_ids: set[str] = set()
    memberships: dict[str, list[dict[str, Any]]] = {item["candidate_id"]: [] for item in candidates}
    if not pilot:
        structural_groups, comparisons, frontier_ids = build_comparison_report(manifest, candidates, results, requests)
        for group in structural_groups:
            for row in group["candidate_rows"]:
                row["metrics"] = _result_metrics(results[row["candidate_id"]])
                candidate = next(item for item in candidates if item["candidate_id"] == row["candidate_id"])
                descriptor = next((member for member in candidate["group_memberships"]
                                   if member["group_id"] == group["manifest_group_id"]
                                   and member["row_index"] == row["row_index"]), None)
                comparison_ids = [comparison["comparison_group_id"] for comparison in comparisons
                                  if comparison["manifest_group_id"] == group["manifest_group_id"]
                                  and comparison["workload_id"] == group["workload_id"]
                                  and comparison["release_stratum_id"] == group["release_stratum_id"]
                                  and row["candidate_id"] in comparison["candidate_ids"]]
                memberships[row["candidate_id"]].append({
                    "manifest_group_id": group["manifest_group_id"],
                    "row_index": row["row_index"],
                    "swept_values": descriptor["swept_row"] if descriptor else row["swept_values"],
                    "coverage_signature_entries": row["coverage_signature"],
                    "recommendation_scope": row["recommendation_scope"],
                    "comparison_group_ids": sorted(comparison_ids),
                    "frontier_member": row["candidate_id"] in frontier_ids,
                })

    provenance_stable = (calculate_source_hashes(root, source_paths(manifest)) == source_hashes
                         and file_sha256(binary_path.resolve()) == candidates[0]["simulator_build_identity"])
    if not provenance_stable:
        for comparison in comparisons:
            comparison["frontier_candidate_ids"] = []
            comparison["frontier_status"] = "withheld_provenance_changed"
        frontier_ids.clear()

    trace_ids = list(dict.fromkeys([*baseline_ids, *sorted(frontier_ids)]))
    trace_index_records: list[dict[str, Any]] = []
    trace_replay_hosts: list[dict[str, Any]] = []
    retained_trace_bytes = 0
    trace_replay_failed = False
    candidate_by_id = {item["candidate_id"]: item for item in candidates}
    for candidate_id in trace_ids:
        candidate = candidate_by_id[candidate_id]
        first_result = results[candidate_id]
        if first_result.get("execution_status") not in ("completed", "censored"):
            trace_index_records.append({"candidate_id": candidate_id, "status": "not_attempted",
                                        "reason": "first_pass_has_no_replayable_measurement"})
            continue
        if not manifest["trace_policy"]["baseline_trace_replay"] and candidate_id in baseline_ids:
            trace_index_records.append({"candidate_id": candidate_id, "status": "not_requested", "reason": "trace_policy_disabled"})
            continue
        trace_record, retained_trace_bytes, diagnostic = _trace_replay(
            root, binary_path.resolve(), output_root, candidate, requests[candidate_id], first_result,
            manifest, output_cap, summary_reserve, retained_trace_bytes)
        trace_index_records.append({"candidate_id": candidate_id, **trace_record})
        trace_replay_hosts.append(diagnostic)
        _append_host_diagnostic(output_root, diagnostic)
        if trace_record.get("status") == "failed":
            trace_replay_failed = True
    trace_index = {"schema_id": "axissim.study-trace-index.v1", "records": trace_index_records,
                   "retained_trace_bytes": retained_trace_bytes,
                   "trace_cap_bytes": manifest["limits"]["max_retained_trace_bytes"]}
    if not _write_output_file(output_root, output_root / "trace-index.json", canonical_json(trace_index) + b"\n",
                              output_cap, summary_reserve):
        raise StudyError("trace index exceeds max_study_output_bytes")

    for comparison in comparisons:
        if not comparison["frontier_candidate_ids"]:
            continue
        bucket = "covered" if comparison["frontier_kind"] == "provisional_covered" else "exploratory"
        csv_path = output_root / "pareto" / bucket / f"{comparison['comparison_group_id']}.csv"
        if not _write_output_file(output_root, csv_path, _csv_bytes(comparison, requests, results),
                                  output_cap, summary_reserve):
            raise StudyError("Pareto CSV exceeds max_study_output_bytes")

    workload_by_id = {item["workload_id"]: item for item in manifest["workloads"]}
    workload_reports = []
    for workload_id, workload in workload_by_id.items():
        baseline_id = next((candidate_id for candidate_id in baseline_ids
                            if candidate_by_id[candidate_id]["workload_id"] == workload_id), None)
        report, markdown = _workload_report(workload, candidates, results, baseline_id,
                                             structural_groups, comparisons, planning_diagnostics, pilot)
        workload_reports.append(report)
        if not _write_output_file(output_root, output_root / "workload-reports" / f"{workload_id}.json",
                                  canonical_json(report) + b"\n", output_cap, summary_reserve):
            raise StudyError("workload report exceeds max_study_output_bytes")
        if not _write_output_file(output_root, output_root / "workload-reports" / f"{workload_id}.md",
                                  markdown.encode("utf-8"), output_cap, summary_reserve):
            raise StudyError("workload Markdown report exceeds max_study_output_bytes")

    outcome_by_workload = {
        workload_id: _candidate_status_counts([results[item["candidate_id"]] for item in candidates
                                               if item["workload_id"] == workload_id])
        for workload_id in workload_by_id
    }
    budget_by_workload = {
        workload_id: {status: sum(results[item["candidate_id"]].get("power_budget_status", "not_evaluated") == status
                                  for item in candidates if item["workload_id"] == workload_id)
                      for status in ("pass", "fail", "not_evaluated")}
        for workload_id in workload_by_id
    }
    budget_totals = {status: sum(row[status] for row in budget_by_workload.values())
                     for status in ("pass", "fail", "not_evaluated")}
    totals = {key: sum(value[key] for value in outcome_by_workload.values())
              for key in ("completed", "invalid", "failed", "censored", "not_run")}
    checked_ids = selected_ids if pilot else [candidate["candidate_id"] for candidate in candidates]
    incomplete = bool(not_run or any(_result_status(results[candidate_id]) != "completed"
                                     for candidate_id in checked_ids))
    run_status = "partial" if incomplete or not provenance_stable or trace_replay_failed else "completed"
    memberships = {key: sorted(value, key=lambda item: (item["manifest_group_id"], item["row_index"]))
                   for key, value in memberships.items()}
    candidate_records = []
    for candidate in candidates:
        candidate_id = candidate["candidate_id"]
        has_request = (output_root / "requests" / f"{candidate_id}.json").is_file()
        candidate_records.append(_candidate_summary_record(
            candidate, results[candidate_id], requests[candidate_id],
            f"results/{candidate_id}.json" if has_request else None,
            f"requests/{candidate_id}.json" if has_request else None,
            memberships[candidate_id]))

    timing_summary = _candidate_wall_timing_summary(
        preflight["estimate"]["projected_candidate_wall_seconds_including_full_trace_replay"],
        first_pass_hosts, trace_replay_hosts)
    _append_host_diagnostic(output_root, {
        "phase": "first_pass_resource_usage",
        **timing_summary,
    })

    summary = {
        "schema_id": "axissim.study-summary.v1",
        "schema_version": 1,
        "study_status": ("pilot" if pilot and run_status == "completed"
                         else "partial" if pilot else run_status),
        "manifest_id": digest(manifest),
        "planning_report_identity": planning["plan_id"],
        "preflight_report_identity": digest(preflight),
        "simulator_build_identity": candidates[0]["simulator_build_identity"],
        "candidate_count_selected": len(candidates),
        "candidate_count_attempted": len(candidates) - totals["not_run"],
        "candidate_outcomes": totals,
        "candidate_outcomes_by_workload": outcome_by_workload,
        "modeled_budget_outcomes": budget_totals,
        "modeled_budget_outcomes_by_workload": budget_by_workload,
        "planning_diagnostics": planning_diagnostics,
        "preflight_estimates": preflight["estimate"],
        "power_budget_w": manifest["power_budget_w"],
        "power_feasibility_label": "Provisionally feasible under current modeled power terms.",
        "candidate_records": candidate_records,
        "structural_groups": structural_groups,
        "pareto_groups": comparisons,
        "frontier_candidate_ids": sorted(frontier_ids) if provenance_stable and not pilot else [],
        "trace_index_path": "trace-index.json",
        "trace_replay_failure_count": sum(record.get("status") == "failed" for record in trace_index_records),
        "workload_reports": [f"workload-reports/{item['workload_id']}.json" for item in workload_reports],
        "canonical_artifacts": _canonical_artifact_inventory(output_root),
        "limitations": [
            "C++ is authoritative for simulated metrics, energy, power, and modeled budget status.",
            "Uniform task-energy interpolation over execution intervals is an accounting approximation, not an activity trace.",
            "DRAM-interface bandwidth scaling and SRAM access/resident-weight costs are incomplete or unpriced; affected configurations are exploratory.",
            "Arm64/x86 host evidence is not a direct prediction of hypothetical target hardware.",
            "No physical silicon feasibility, measured chip power, or global architectural optimum is claimed.",
        ],
    }
    summary_bytes = canonical_json(summary) + b"\n"
    if _retained_bytes(output_root) + len(summary_bytes) > output_cap:
        raise StudyError("final study summary exceeds max_study_output_bytes")
    atomic_write(output_root / "study-summary.json", summary_bytes)
    actual_output_bytes = _retained_bytes(output_root)
    if actual_output_bytes > output_cap:
        raise StudyError("final study artifacts exceed max_study_output_bytes")
    resource_usage = {
        "schema_id": "axissim.study-host-resource-diagnostics.v1",
        "python_version": platform.python_version(),
        "platform": platform.platform(),
        **timing_summary,
        "actual_study_wall_seconds": Decimal(str(time.perf_counter() - wall_started)),
        "preflight_worst_case_output_bytes": estimated_output,
        "actual_retained_study_output_bytes": actual_output_bytes,
        "configured_study_output_cap_bytes": output_cap,
        "available_disk_bytes_after_study": shutil.disk_usage(output_root).free,
        "host_diagnostics_excluded_from_study_output_cap": True,
    }
    atomic_write(output_root / "host-diagnostics" / "resource-usage.json", canonical_json(resource_usage) + b"\n")
    return summary, 0 if run_status == "completed" else 1


def _canonical_study_files(directory: pathlib.Path) -> dict[str, bytes]:
    return {path.relative_to(directory).as_posix(): path.read_bytes()
            for path in sorted(directory.rglob("*"), key=lambda item: item.relative_to(directory).as_posix().encode("utf-8"))
            if path.is_file() and "host-diagnostics" not in path.relative_to(directory).parts
            and ".staging" not in path.relative_to(directory).parts}


def reproduce_study(root: pathlib.Path, manifest_path: pathlib.Path, binary_path: pathlib.Path,
                    plan_report_path: pathlib.Path, preflight_path: pathlib.Path,
                    reference_root: pathlib.Path, output_root: pathlib.Path) -> tuple[dict[str, Any], int]:
    reference_root = reference_root.resolve()
    reference = load_json(reference_root / "study-summary.json")
    if reference.get("schema_id") != "axissim.study-summary.v1" or reference.get("study_status") != "completed":
        raise StudyError("reproduction reference is not a complete study")
    reproduced, execution_code = execute_study(root, manifest_path, binary_path,
                                               plan_report_path, preflight_path,
                                               output_root, pilot=False)
    expected = _canonical_study_files(reference_root)
    actual = _canonical_study_files(output_root.resolve())
    differing = set(expected) ^ set(actual)
    differing.update(path for path in set(expected) & set(actual) if expected[path] != actual[path])
    report = {
        "schema_id": "axissim.study-reproduction-check.v1",
        "reference_manifest_id": reference.get("manifest_id"),
        "reproduced_manifest_id": reproduced.get("manifest_id"),
        "reference_simulator_build_identity": reference.get("simulator_build_identity"),
        "reproduced_simulator_build_identity": reproduced.get("simulator_build_identity"),
        "canonical_artifacts_identical": not differing,
        "mismatched_paths": sorted(differing, key=lambda item: item.encode("utf-8")),
    }
    atomic_write(output_root.resolve() / "reproduction-check.json", canonical_json(report) + b"\n")
    return report, 0 if execution_code == 0 and not differing else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "preflight"):
        sub = subparsers.add_parser(name)
        sub.add_argument("--manifest", required=True, type=pathlib.Path)
        sub.add_argument("--binary", required=True, type=pathlib.Path)
        sub.add_argument("--report", required=True, type=pathlib.Path)
        if name == "preflight":
            sub.add_argument("--plan-report", type=pathlib.Path)
    for name in ("execute", "reproduce"):
        sub = subparsers.add_parser(name)
        sub.add_argument("--manifest", required=True, type=pathlib.Path)
        sub.add_argument("--binary", required=True, type=pathlib.Path)
        sub.add_argument("--plan-report", required=True, type=pathlib.Path)
        sub.add_argument("--preflight-report", required=True, type=pathlib.Path)
        if name == "execute":
            sub.add_argument("--output-dir", required=True, type=pathlib.Path)
            sub.add_argument("--pilot", action="store_true", help="run the three-workload baseline pilot only")
        else:
            sub.add_argument("--reference-study", required=True, type=pathlib.Path)
            sub.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    root = pathlib.Path(__file__).resolve().parents[1]
    try:
        manifest_path = args.manifest if args.manifest.is_absolute() else root / args.manifest
        binary_path = args.binary if args.binary.is_absolute() else root / args.binary
        if args.command == "plan":
            report_path = args.report if args.report.is_absolute() else root / args.report
            result = plan(root, manifest_path, binary_path, report_path)
        elif args.command == "preflight":
            report_path = args.report if args.report.is_absolute() else root / args.report
            manifest = load_json(manifest_path)
            plan_report_arg = args.plan_report or pathlib.Path(manifest.get("planning_report_path", ""))
            plan_report_path = plan_report_arg if plan_report_arg.is_absolute() else root / plan_report_arg
            result = preflight(root, manifest_path, binary_path, report_path, plan_report_path)
            print(f"{result['status']}: {result.get('candidate_count', 0)} candidates; report {report_path}")
            return 0 if result["status"] == "ready" else 2
        else:
            plan_report_path = args.plan_report if args.plan_report.is_absolute() else root / args.plan_report
            preflight_path = args.preflight_report if args.preflight_report.is_absolute() else root / args.preflight_report
            output_root = args.output_dir if args.output_dir.is_absolute() else root / args.output_dir
            if args.command == "execute":
                result, code = execute_study(root, manifest_path, binary_path, plan_report_path,
                                             preflight_path, output_root, args.pilot)
                print(f"{result['study_status']}: {result['candidate_count_attempted']} candidate evaluations; output {output_root}")
                return code
            reference_root = args.reference_study if args.reference_study.is_absolute() else root / args.reference_study
            result, code = reproduce_study(root, manifest_path, binary_path, plan_report_path,
                                           preflight_path, reference_root, output_root)
            print(f"{'identical' if result['canonical_artifacts_identical'] else 'different'}: {len(result['mismatched_paths'])} canonical artifact differences; output {output_root}")
            return code
        print(f"{result['status']}: {result.get('candidate_count_unique', 0)} candidates; report {report_path}")
        return 0 if result["status"] in ("planned", "planned_with_invalid_rows") else 2
    except (StudyError, KeyError, TypeError, ValueError, OSError) as error:
        failure = {"schema_id": "axissim.study-preflight-report.v1", "status": "rejected",
                   "structured_status": "rejected", "exit_code": 2, "rejection_reasons": [str(error)],
                   "candidate_count": 0, "candidate_simulations_launched": 0, "study_evaluate_invocations": 0}
        try:
            if hasattr(args, "report"):
                args.report.parent.mkdir(parents=True, exist_ok=True)
                args.report.write_bytes(canonical_json(failure) + b"\n")
        except (OSError, AttributeError):
            pass
        print(f"rejected: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
