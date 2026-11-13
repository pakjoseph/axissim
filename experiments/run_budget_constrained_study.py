#!/usr/bin/env python3
"""Plan and preflight the opt-in AxisSim budget-constrained study (M4).

This module intentionally does not execute study candidates or compute model
measurements. C++ remains authoritative for simulation, energy, power, and
budget status. Python owns deterministic enumeration and planning only here.
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
    summary_reserve = 4096 + 1024 * len(candidates) + 512 * sum(len(candidate["group_memberships"]) for candidate in candidates)
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


def evaluator_outcome_is_accepted(return_code: int, structured_status: str) -> bool:
    """Future execute-boundary guard: process success and JSON success are both required."""
    return return_code == 0 and structured_status == "completed"


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
    args = parser.parse_args(argv)
    root = pathlib.Path(__file__).resolve().parents[1]
    try:
        manifest_path = args.manifest if args.manifest.is_absolute() else root / args.manifest
        binary_path = args.binary if args.binary.is_absolute() else root / args.binary
        report_path = args.report if args.report.is_absolute() else root / args.report
        if args.command == "plan":
            result = plan(root, manifest_path, binary_path, report_path)
        else:
            manifest = load_json(manifest_path)
            plan_report_arg = args.plan_report or pathlib.Path(manifest.get("planning_report_path", ""))
            plan_report_path = plan_report_arg if plan_report_arg.is_absolute() else root / plan_report_arg
            result = preflight(root, manifest_path, binary_path, report_path, plan_report_path)
        print(f"{result['status']}: {result.get('candidate_count_unique', result.get('candidate_count', 0))} candidates; report {args.report}")
        return 0 if result["status"] in ("planned", "planned_with_invalid_rows", "ready") else 2
    except (StudyError, KeyError, TypeError, ValueError, OSError) as error:
        failure = {"schema_id": "axissim.study-preflight-report.v1", "status": "rejected",
                   "structured_status": "rejected", "exit_code": 2, "rejection_reasons": [str(error)],
                   "candidate_count": 0, "candidate_simulations_launched": 0, "study_evaluate_invocations": 0}
        try:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_bytes(canonical_json(failure) + b"\n")
        except (OSError, AttributeError):
            pass
        print(f"rejected: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
