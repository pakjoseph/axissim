#!/usr/bin/env python3
"""Bounded orchestration for the paired GEMM C++ evaluator (Python 3.10+)."""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import xml.sax.saxutils as xml_escape
from pathlib import Path, PurePosixPath
from typing import Any, Callable

SCHEMA = "axissim.gemm-pair-study.manifest"
CANONICALIZATION = "axissim-json-canonical-v1"
REQUEST_SCHEMA = "axissim.gemm-pair.request"
RESULT_SCHEMA = "axissim.gemm-pair.result"
STUDY_SOURCE_ID = "axissim.gemm-pair-study.source-context"
STUDY_ID_SCHEMA = "axissim.gemm-pair-study.identity"
MAX_SAFE = 9_007_199_254_740_991
STATUS_VALUES = {
    "complete", "duplicate", "invalid", "partial", "failed", "timeout",
    "process_error", "missing_result", "identity_mismatch",
    "result_schema_error", "output_limit", "not_started_due_to_limit",
    "not_started_provenance_failure", "not_started_due_to_preflight_failure",
}
CSV_COLUMNS = [
    "source_row_index", "study_row_id", "candidate_pair_id",
    "comparison_group_id", "shape_id", "capacity_level_id", "runner_status",
    "cpp_status", "rvv_arm_status", "matrix_arm_status",
    "rvv_compute_cycles_per_unit", "matrix_compute_cycles_per_unit",
    "rvv_read_a_bytes_per_unit", "rvv_read_b_bytes_per_unit",
    "rvv_write_c_bytes_per_unit", "matrix_read_a_bytes_per_unit",
    "matrix_read_b_bytes_per_unit", "matrix_write_c_bytes_per_unit",
    "error_codes",
]
CSV_COLUMNS_V2 = [
    "schema_id", "schema_version", "manifest_id", "source_row_index",
    "study_row_id", "candidate_pair_id", "comparison_group_id", "shape_id",
    "capacity_level_id", "base_case_id", "sensitivity_case_id", "workload_id",
    "rvv_mapping_id", "matrix_mapping_id", "request_sha256",
    "model_assumptions_sha256", "runner_status", "cpp_status",
    "rvv_arm_status", "matrix_arm_status", "bandwidth_bytes_per_second",
    "batch_setup_cycles", "rvv_compute_cycles_per_unit",
    "rvv_serialized_read_cycles_per_unit",
    "rvv_serialized_compute_cycles_per_unit",
    "rvv_serialized_write_cycles_per_unit",
    "rvv_serialized_total_cycles_per_unit", "matrix_compute_cycles_per_unit",
    "matrix_serialized_read_cycles_per_unit",
    "matrix_serialized_compute_cycles_per_unit",
    "matrix_serialized_write_cycles_per_unit",
    "matrix_serialized_total_cycles_per_unit", "rvv_read_a_bytes_per_unit",
    "rvv_read_b_bytes_per_unit", "rvv_write_c_bytes_per_unit",
    "matrix_read_a_bytes_per_unit", "matrix_read_b_bytes_per_unit",
    "matrix_write_c_bytes_per_unit", "error_codes",
]
HYPOTHESES = [
    {
        "hypothesis_id": "capacity_sensitivity",
        "statement": "At fixed shape, increasing matched abstract MAC slots per cycle from 4 to 8 reduces analytical compute cycles for at least one mapping.",
    },
    {
        "hypothesis_id": "mapping_traffic_difference",
        "statement": "For at least one shape, the two mappings report different logical A/B/C payload bytes.",
    },
]
HYPOTHESES_V2 = [
    {"hypothesis_id": "H1", "statement": "For at least one fixed base case and sensitivity point, RVV and matrix report different serialized read-plus-write service cycles because their logical payload or batch counts differ."},
    {"hypothesis_id": "H2", "statement": "Moving from P4 to P8 reduces the same mapping's compute-only cycles and serialized total cycles for at least one fixed shape and sensitivity setting."},
    {"hypothesis_id": "H3", "statement": "At fixed shape, capacity, setup, and mapping, higher declared bandwidth never increases read service, write service, or total serialized cycles; at least one service result decreases strictly."},
    {"hypothesis_id": "H4", "statement": "At fixed shape, capacity, bandwidth, and mapping, increasing setup from zero to one cycle per batch never decreases total cycles, and increases totals wherever returned A/B/C batch counts are nonzero."},
    {"hypothesis_id": "H5", "statement": "At least one selected case has a strict compute-cycle winner that is not the strict serialized-total winner."},
]
FIXED_LIMITS = {
    "max_active_seconds": 900,
    "max_bundle_bytes": 2_097_152,
    "max_diagnostics_bytes_per_invocation": 4096,
    "max_evaluator_seconds": 30,
    "max_manifest_bytes": 65536,
    "max_request_file_bytes": 65536,
    "max_result_payload_bytes": 65536,
    "max_source_rows": 48,
    "max_unique_candidates": 24,
}
V2_LIMITS = {**FIXED_LIMITS, "max_result_payload_bytes": 8192}
M63C_V1_MANIFEST_SHA256 = "0c5fa201e0b6816fcf4b15d6c96094aca150ba5fd2f85f0d464c283e55816d23"
V2_SENSITIVITY_CASES = [
    {"sensitivity_case_id": "bw_1e9_bytes_per_second_setup_0", "bandwidth_bytes_per_second": 1_000_000_000, "batch_setup_cycles": 0},
    {"sensitivity_case_id": "bw_64e9_bytes_per_second_setup_0", "bandwidth_bytes_per_second": 64_000_000_000, "batch_setup_cycles": 0},
    {"sensitivity_case_id": "bw_1e9_bytes_per_second_setup_1", "bandwidth_bytes_per_second": 1_000_000_000, "batch_setup_cycles": 1},
    {"sensitivity_case_id": "bw_64e9_bytes_per_second_setup_1", "bandwidth_bytes_per_second": 64_000_000_000, "batch_setup_cycles": 1},
]
V2_BASELINE_REFERENCE = {
    "study_label": "paired-gemm-v1",
    "manifest_id": "d570caccdfc4f7b0136fe6b1ab7db512c00b182a80fe8b5b4a16353675754b07",
    "canonical_artifact_manifest_sha256": "490c6623867b7adf9c3f0e3db7ff5d7a78341e24286eab06e572ccac6b7c0401",
    "source_revision": "b7ce0edfa2775487275abeff8e3db18fa52d97cf",
    "study_run_id": "308735741148488aa92cee059d241aa6430ff292d8ed385c3f23c59a05e31f0e",
    "bundle_tar_sha256": "6e7c5733ce069de7d169ad3cc0ffdbcf86fea458325efab69a345ec232281f42",
}
FILE_CAPS = {
    "manifest.json": 65537,
    "plan.json": 65536,
    "preflight.json": 16384,
    "provenance.json": 16384,
    "runner-ledger.jsonl": 98304,
    "summary.json": 262144,
    "summary.csv": 65536,
    "report.md": 32768,
    "plots/compute-cycles.svg": 65536,
    "plots/logical-payload-bytes.svg": 65536,
    "canonical-artifact-manifest.json": 65536,
    "host-diagnostics.jsonl": 24 * 4096 + 2 * 4096,
    "bundle-index.json": 8192,
}
V2_FILE_CAPS = {
    **FILE_CAPS,
    "plots/serialized-total-cycles.svg": 65536,
    "plots/serialized-cycle-breakdown.svg": 65536,
}
MAX_STAGING = FILE_CAPS["summary.json"]


class RunnerError(Exception):
    def __init__(self, code: str, message: str, *, exit_code: int = 4, path: str = ""):
        super().__init__(message)
        self.code = code
        self.exit_code = exit_code
        self.path = path


def _fail(code: str, message: str, *, exit_code: int = 2, path: str = "") -> None:
    raise RunnerError(code, message, exit_code=exit_code, path=path)


def _string_bytes(value: str) -> bytes:
    if not isinstance(value, str) or any(ord(ch) > 0x7F for ch in value):
        _fail("invalid_value", "manifest and contract strings must be ASCII")
    out = ['"']
    for ch in value:
        n = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif n < 0x20:
            out.append(f"\\u00{n:02x}")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out).encode("ascii")


def canonical_bytes(value: Any) -> bytes:
    """Implement the C++ axissim-json-canonical-v1 encoding exactly."""
    if value is None or isinstance(value, float):
        _fail("invalid_value", "canonical JSON disallows null and floating values")
    if isinstance(value, bool):
        return b"true" if value else b"false"
    if isinstance(value, int):
        if value < 0 or value > MAX_SAFE:
            _fail("invalid_value", "canonical integer is outside the supported range")
        return str(value).encode("ascii")
    if isinstance(value, str):
        return _string_bytes(value)
    if isinstance(value, list):
        return b"[" + b",".join(canonical_bytes(item) for item in value) + b"]"
    if isinstance(value, dict):
        keys = list(value)
        if any(not isinstance(key, str) or any(ord(ch) > 0x7F for ch in key) for key in keys):
            _fail("invalid_value", "canonical JSON object keys must be ASCII strings")
        keys.sort(key=lambda item: item.encode("ascii"))
        return b"{" + b",".join(
            _string_bytes(key) + b":" + canonical_bytes(value[key]) for key in keys
        ) + b"}"
    _fail("invalid_type", f"unsupported canonical JSON value type: {type(value).__name__}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_hash(value: Any) -> str:
    return sha256_bytes(canonical_bytes(value))


def _pairs_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            _fail("duplicate_key", f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_float(_: str) -> Any:
    _fail("invalid_type", "floating-point JSON values are not supported")


def _reject_constant(_: str) -> Any:
    _fail("invalid_value", "non-standard JSON constants are not supported")


def _validate_tree(value: Any, depth: int = 0) -> None:
    if depth > 32:
        _fail("invalid_value", "JSON nesting exceeds 32 levels")
    if value is None or isinstance(value, float):
        _fail("invalid_value", "null and floating-point values are not supported")
    if isinstance(value, bool):
        return
    if isinstance(value, int):
        if value < 0 or value > MAX_SAFE:
            _fail("invalid_value", "integer is outside the safe unsigned range")
        return
    if isinstance(value, str):
        if any(ord(ch) > 0x7F for ch in value):
            _fail("invalid_value", "all schema strings must be ASCII")
        return
    if isinstance(value, list):
        for item in value:
            _validate_tree(item, depth + 1)
        return
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str) or any(ord(ch) > 0x7F for ch in key):
                _fail("invalid_value", "all schema keys must be ASCII")
            _validate_tree(child, depth + 1)
        return
    _fail("invalid_type", f"unsupported JSON value type: {type(value).__name__}")


def parse_json(raw: bytes, *, max_bytes: int | None = None) -> Any:
    if max_bytes is not None and len(raw) > max_bytes:
        _fail("input_limit", "JSON input exceeds the configured byte cap")
    try:
        decoded = raw.decode("utf-8", errors="strict")
        value = json.loads(
            decoded,
            object_pairs_hook=_pairs_no_duplicates,
            parse_float=_reject_float,
            parse_constant=_reject_constant,
        )
    except RunnerError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
        _fail("invalid_json", f"invalid UTF-8 or JSON: {exc}")
    _validate_tree(value)
    return value


def read_json(path: Path, *, max_bytes: int | None = None) -> tuple[Any, bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        _fail("io_error", f"cannot read {path.name}: {exc}", exit_code=4)
    return parse_json(raw, max_bytes=max_bytes), raw


def exact_keys(value: Any, expected: set[str], name: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        actual = set(value) if isinstance(value, dict) else set()
        _fail("invalid_manifest", f"{name} keys must exactly match schema; missing={sorted(expected-actual)}, extra={sorted(actual-expected)}")
    return value


def require_int(value: Any, name: str, *, minimum: int = 0, maximum: int = MAX_SAFE) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        _fail("invalid_manifest", f"{name} must be an integer in [{minimum}, {maximum}]")
    return value


def require_bool(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        _fail("invalid_manifest", f"{name} must be a boolean")
    return value


def require_string(value: Any, name: str, *, expected: str | None = None) -> str:
    if not isinstance(value, str) or (expected is not None and value != expected):
        _fail("invalid_manifest", f"{name} has an unsupported value")
    return value


def validate_manifest(manifest: Any) -> dict[str, Any]:
    if isinstance(manifest, dict) and type(manifest.get("schema_version")) is int and manifest.get("schema_version") == 2:
        return validate_manifest_v2(manifest)
    root_keys = {
        "schema_id", "schema_version", "canonicalization_id", "study_label", "purpose",
        "evaluator", "workload", "measurement", "mapping_constants", "shapes",
        "capacity_levels", "comparison_groups", "hypotheses", "limits",
    }
    m = exact_keys(manifest, root_keys, "manifest")
    require_string(m["schema_id"], "schema_id", expected=SCHEMA)
    require_int(m["schema_version"], "schema_version", minimum=1, maximum=1)
    require_string(m["canonicalization_id"], "canonicalization_id", expected=CANONICALIZATION)
    require_string(m["study_label"], "study_label", expected="paired-gemm-v1")
    require_string(m["purpose"], "purpose", expected="Compare RVV and tiled-matrix GEMM mappings under shape-only assumptions.")
    e = exact_keys(m["evaluator"], {"command", "request_schema_id", "request_schema_version", "result_schema_id", "result_schema_version"}, "evaluator")
    for key, expected in {
        "command": "study-gemm-pair", "request_schema_id": REQUEST_SCHEMA,
        "result_schema_id": RESULT_SCHEMA,
    }.items():
        require_string(e[key], f"evaluator.{key}", expected=expected)
    require_int(e["request_schema_version"], "request_schema_version", minimum=1, maximum=1)
    require_int(e["result_schema_version"], "result_schema_version", minimum=1, maximum=1)
    work = exact_keys(m["workload"], {
        "operation", "batch_count", "input_format", "accumulator_output_format", "overflow",
        "rounding", "layout", "transpose_a", "transpose_b", "initial_c", "operand_data_identity",
    }, "workload")
    fixed_work = {
        "operation": "gemm", "batch_count": 1, "input_format": "signed_i16_twos_complement",
        "accumulator_output_format": "signed_i32_twos_complement", "overflow": "wrap_mod_2pow32",
        "rounding": "none", "layout": "row_major_contiguous", "transpose_a": False,
        "transpose_b": False, "initial_c": "zero", "operand_data_identity": "shape_only_no_values_v1",
    }
    require_int(work["batch_count"], "workload.batch_count", minimum=1)
    require_bool(work["transpose_a"], "workload.transpose_a")
    require_bool(work["transpose_b"], "workload.transpose_b")
    if work != fixed_work:
        _fail("invalid_manifest", "workload semantics must match the approved v1 work unit")
    measure = exact_keys(m["measurement"], {"protocol", "repeat_count", "release_policy", "unit_order", "clock_hz"}, "measurement")
    fixed_measure = {
        "protocol": "finite_serial_batch_v1", "repeat_count": 1, "release_policy": "all_at_zero",
        "unit_order": "ascending_index_no_overlap", "clock_hz": 1_000_000_000,
    }
    require_int(measure["repeat_count"], "measurement.repeat_count", minimum=1)
    require_int(measure["clock_hz"], "measurement.clock_hz", minimum=1)
    if measure != fixed_measure:
        _fail("invalid_manifest", "measurement must match the approved finite serial v1 protocol")
    maps = exact_keys(m["mapping_constants"], {"rvv", "matrix"}, "mapping_constants")
    rvv = exact_keys(maps["rvv"], {
        "kind", "vlen_bits", "sew_bits", "lmul", "macs_per_lane_cycle",
        "accumulator_recurrence_cycles", "kernel_setup_cycles", "chunk_setup_cycles",
    }, "mapping_constants.rvv")
    for key in ("vlen_bits", "sew_bits", "lmul", "macs_per_lane_cycle", "accumulator_recurrence_cycles", "kernel_setup_cycles", "chunk_setup_cycles"):
        require_int(rvv[key], f"mapping_constants.rvv.{key}")
    if rvv != {
        "kind": "rvv_row_v1", "vlen_bits": 128, "sew_bits": 16, "lmul": 1,
        "macs_per_lane_cycle": 1, "accumulator_recurrence_cycles": 1,
        "kernel_setup_cycles": 0, "chunk_setup_cycles": 0,
    }:
        _fail("invalid_manifest", "RVV mapping constants differ from the approved v1 assumptions")
    matrix = exact_keys(maps["matrix"], {"kind", "accumulator_recurrence_cycles", "kernel_setup_cycles", "tile_setup_cycles"}, "mapping_constants.matrix")
    for key in ("accumulator_recurrence_cycles", "kernel_setup_cycles", "tile_setup_cycles"):
        require_int(matrix[key], f"mapping_constants.matrix.{key}")
    if matrix != {
        "kind": "tiled_matrix_output_stationary_v1", "accumulator_recurrence_cycles": 1,
        "kernel_setup_cycles": 0, "tile_setup_cycles": 0,
    }:
        _fail("invalid_manifest", "matrix mapping constants differ from the approved v1 assumptions")
    shapes = m["shapes"]
    if not isinstance(shapes, list) or not 1 <= len(shapes) <= 24:
        _fail("invalid_manifest", "shapes must contain 1 through 24 entries")
    shape_ids: set[str] = set()
    shape_pattern = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
    shape_keys = {"shape_id", "m", "n", "k", "tile_m", "tile_n", "tile_k", "local_capacity_bytes"}
    shape_by_id: dict[str, dict[str, Any]] = {}
    for i, item in enumerate(shapes):
        sh = exact_keys(item, shape_keys, f"shapes[{i}]")
        sid = require_string(sh["shape_id"], f"shapes[{i}].shape_id")
        if not shape_pattern.fullmatch(sid) or sid in shape_ids:
            _fail("invalid_manifest", f"invalid or duplicate shape_id at shapes[{i}]")
        shape_ids.add(sid)
        for key in shape_keys - {"shape_id"}:
            require_int(sh[key], f"shapes[{i}].{key}", minimum=1)
        shape_by_id[sid] = sh
    levels = m["capacity_levels"]
    if not isinstance(levels, list) or len(levels) != 2:
        _fail("invalid_manifest", "capacity_levels must contain exactly p4 and p8")
    expected_levels = [{"level_id": "p4", "mac_slots_per_cycle": 4}, {"level_id": "p8", "mac_slots_per_cycle": 8}]
    if levels != expected_levels:
        _fail("invalid_manifest", "capacity levels must be the approved ordered p4/p8 pair")
    groups = m["comparison_groups"]
    if not isinstance(groups, list) or not 1 <= len(groups) <= 24:
        _fail("invalid_manifest", "comparison_groups must contain 1 through 24 entries")
    if len(groups) * 2 > FIXED_LIMITS["max_source_rows"]:
        _fail("candidate_limit", "manifest expansion exceeds 48 source rows")
    group_ids: set[str] = set()
    for i, item in enumerate(groups):
        group = exact_keys(item, {"group_id", "shape_id", "capacity_level_ids", "pilot_level_id"}, f"comparison_groups[{i}]")
        gid = require_string(group["group_id"], f"comparison_groups[{i}].group_id")
        if not shape_pattern.fullmatch(gid) or gid in group_ids:
            _fail("invalid_manifest", f"invalid or duplicate group_id at comparison_groups[{i}]")
        group_ids.add(gid)
        if group["shape_id"] not in shape_by_id:
            _fail("invalid_manifest", f"unknown shape in comparison_groups[{i}]")
        if group["capacity_level_ids"] != ["p4", "p8"] or group["pilot_level_id"] != "p4":
            _fail("invalid_manifest", f"comparison_groups[{i}] must vary p4/p8 and pilot p4")
    if m["hypotheses"] != HYPOTHESES:
        _fail("invalid_manifest", "hypotheses must be the two fixed approved v1 statements")
    if m["limits"] != FIXED_LIMITS:
        _fail("invalid_manifest", "limits must match the fixed operational ceilings")
    return m


def validate_manifest_v2(manifest: Any) -> dict[str, Any]:
    root_keys = {
        "schema_id", "schema_version", "canonicalization_id", "study_label", "purpose",
        "evaluator", "workload", "measurement", "mapping_constants", "shapes",
        "capacity_levels", "comparison_groups", "sensitivity_cases", "baseline_reference",
        "hypotheses", "limits",
    }
    m = exact_keys(manifest, root_keys, "manifest")
    require_string(m["schema_id"], "schema_id", expected=SCHEMA)
    require_int(m["schema_version"], "schema_version", minimum=2, maximum=2)
    require_string(m["canonicalization_id"], "canonicalization_id", expected=CANONICALIZATION)
    require_string(m["study_label"], "study_label", expected="paired-gemm-serial-sensitivity-v1")
    require_string(m["purpose"], "purpose", expected="Compare RVV and tiled-matrix GEMM mappings under shape-only assumptions.")

    # Pin every inherited workload/mapping choice to the accepted M6.3c manifest.
    base_path = Path(__file__).resolve().parent / "matrix_mapping_study" / "manifest.v1.json"
    try:
        base_raw = base_path.read_bytes()
    except OSError as exc:
        _fail("baseline_missing", f"cannot read accepted M6.3c manifest: {exc}", exit_code=4)
    if sha256_bytes(base_raw) != M63C_V1_MANIFEST_SHA256:
        _fail("baseline_identity_mismatch", "accepted M6.3c manifest hash changed", exit_code=4)
    base = validate_manifest(parse_json(base_raw, max_bytes=FIXED_LIMITS["max_manifest_bytes"]))
    for key in (
        "canonicalization_id", "purpose", "evaluator", "workload", "measurement",
        "mapping_constants", "shapes", "capacity_levels", "comparison_groups",
    ):
        # Python equality aliases booleans and integers (for example True == 1).
        # Compare canonical JSON bytes so the pinned v1 subobjects also retain
        # their exact JSON types in the v2 manifest.
        if canonical_bytes(m[key]) != canonical_bytes(base[key]):
            _fail("invalid_manifest", f"v2 field {key} must exactly match accepted M6.3c manifest v1")

    cases = m["sensitivity_cases"]
    if not isinstance(cases, list) or cases != V2_SENSITIVITY_CASES:
        _fail("invalid_manifest", "sensitivity_cases must match the four approved ordered cases")
    for index, case in enumerate(cases):
        exact_keys(case, {"sensitivity_case_id", "bandwidth_bytes_per_second", "batch_setup_cycles"}, f"sensitivity_cases[{index}]")
        require_string(case["sensitivity_case_id"], f"sensitivity_cases[{index}].sensitivity_case_id")
        require_int(case["bandwidth_bytes_per_second"], f"sensitivity_cases[{index}].bandwidth_bytes_per_second", minimum=1)
        require_int(case["batch_setup_cycles"], f"sensitivity_cases[{index}].batch_setup_cycles")
    if m["baseline_reference"] != V2_BASELINE_REFERENCE:
        _fail("invalid_manifest", "baseline_reference differs from the accepted M6.3c identities")
    exact_keys(m["baseline_reference"], set(V2_BASELINE_REFERENCE), "baseline_reference")
    if m["hypotheses"] != HYPOTHESES_V2:
        _fail("invalid_manifest", "hypotheses must match the five preregistered M6.3d checks")
    for index, hypothesis in enumerate(m["hypotheses"]):
        exact_keys(hypothesis, {"hypothesis_id", "statement"}, f"hypotheses[{index}]")
    if m["limits"] != V2_LIMITS:
        _fail("invalid_manifest", "limits must match the fixed M6.3d operational ceilings")
    return m


def _limits_for_manifest(manifest: dict[str, Any]) -> dict[str, int]:
    return V2_LIMITS if manifest.get("schema_version") == 2 else FIXED_LIMITS


def _caps_for_manifest(manifest: dict[str, Any]) -> dict[str, int]:
    return V2_FILE_CAPS if manifest.get("schema_version") == 2 else FILE_CAPS


def _caps_for_row(row: dict[str, Any]) -> dict[str, int]:
    return V2_FILE_CAPS if row.get("study_schema_version") == 2 else FILE_CAPS


def _request_for(manifest: dict[str, Any], shape: dict[str, Any], p: int) -> dict[str, Any]:
    workload = dict(manifest["workload"])
    workload.update({key: shape[key] for key in ("m", "n", "k")})
    rvv = dict(manifest["mapping_constants"]["rvv"])
    rvv["lanes"] = p
    matrix = dict(manifest["mapping_constants"]["matrix"])
    matrix.update({key: shape[key] for key in ("tile_m", "tile_n", "tile_k", "local_capacity_bytes")})
    matrix["mac_slots_per_cycle"] = p
    return {
        "schema_id": REQUEST_SCHEMA,
        "schema_version": 1,
        "canonicalization_id": CANONICALIZATION,
        "workload": workload,
        "measurement": dict(manifest["measurement"]),
        "mappings": {"rvv": rvv, "matrix": matrix},
    }


def _request_ids(request: dict[str, Any]) -> dict[str, str]:
    w_id = canonical_hash(request["workload"])
    rvv_id = canonical_hash(request["mappings"]["rvv"])
    matrix_id = canonical_hash(request["mappings"]["matrix"])
    pair_identity = {
        "schema_id": request["schema_id"],
        "schema_version": request["schema_version"],
        "workload_id": w_id,
        "measurement": request["measurement"],
        "mappings": {"rvv": rvv_id, "matrix": matrix_id},
    }
    assumptions = {
        "schema_id": "axissim.gemm-pair.model-assumptions",
        "schema_version": 1,
        "workload_id": w_id,
        "measurement": request["measurement"],
        "model_semantics": {
            "rvv": "axissim.rvv_row_v1",
            "matrix": "axissim.tiled_matrix_output_stationary_v1",
        },
        "mappings": request["mappings"],
    }
    return {
        "request_sha256": canonical_hash(request),
        "workload_id": w_id,
        "rvv_mapping_id": rvv_id,
        "matrix_mapping_id": matrix_id,
        "candidate_pair_id": canonical_hash(pair_identity),
        "model_assumptions_sha256": canonical_hash(assumptions),
    }


def expand_candidates(manifest: dict[str, Any], manifest_id: str) -> list[dict[str, Any]]:
    if manifest.get("schema_version") == 2:
        return expand_candidates_v2(manifest, manifest_id)
    shapes = {item["shape_id"]: item for item in manifest["shapes"]}
    rows: list[dict[str, Any]] = []
    first_for_request: dict[bytes, int] = {}
    for group in manifest["comparison_groups"]:
        shape = shapes[group["shape_id"]]
        for level_id in group["capacity_level_ids"]:
            p = 4 if level_id == "p4" else 8
            req = _request_for(manifest, shape, p)
            req_bytes = canonical_bytes(req)
            request_file = req_bytes + b"\n"
            if len(request_file) > FIXED_LIMITS["max_request_file_bytes"]:
                _fail("request_limit", "generated request exceeds C++ raw-input limit")
            req_sha = sha256_bytes(req_bytes)
            index = len(rows)
            row_preimage = {
                "schema_id": "axissim.gemm-pair-study.row",
                "schema_version": 1,
                "manifest_id": manifest_id,
                "comparison_group_id": group["group_id"],
                "source_row_index": index,
                "request_sha256": req_sha,
            }
            row_id = canonical_hash(row_preimage)
            ids = _request_ids(req)
            duplicate_of = first_for_request.get(req_bytes)
            if duplicate_of is None:
                first_for_request[req_bytes] = index
            result_path = f"results/{ids['candidate_pair_id']}.result.json"
            rows.append({
                "study_row_id": row_id,
                "comparison_group_id": group["group_id"],
                "source_row_index": index,
                "shape_id": shape["shape_id"],
                "capacity_level_id": level_id,
                "request_sha256": req_sha,
                "candidate_pair_id": ids["candidate_pair_id"],
                "duplicate_of": duplicate_of,
                "request": req,
                "request_bytes": request_file,
                "ids": ids,
                "request_path": f"requests/{ids['candidate_pair_id']}.request.json",
                "result_path": result_path,
            })
    unique = {row["request_bytes"] for row in rows}
    if len(unique) > FIXED_LIMITS["max_unique_candidates"]:
        _fail("candidate_limit", "manifest expansion exceeds 24 unique paired requests")
    return rows


def expand_candidates_v2(manifest: dict[str, Any], manifest_id: str) -> list[dict[str, Any]]:
    shapes = {item["shape_id"]: item for item in manifest["shapes"]}
    sensitivities = manifest["sensitivity_cases"]
    rows: list[dict[str, Any]] = []
    first_for_request: dict[bytes, int] = {}
    for group in manifest["comparison_groups"]:
        shape = shapes[group["shape_id"]]
        for level_id in group["capacity_level_ids"]:
            p = 4 if level_id == "p4" else 8
            for sensitivity in sensitivities:
                req = _request_for(manifest, shape, p)
                req["measurement"]["backing_store_sensitivity"] = {
                    "bandwidth_bytes_per_second": sensitivity["bandwidth_bytes_per_second"],
                    "batch_setup_cycles": sensitivity["batch_setup_cycles"],
                }
                req_bytes = canonical_bytes(req)
                request_file = req_bytes + b"\n"
                if len(request_file) > manifest["limits"]["max_request_file_bytes"]:
                    _fail("request_limit", "generated request exceeds C++ raw-input limit")
                req_sha = sha256_bytes(req_bytes)
                ids = _request_ids(req)
                index = len(rows)
                row_preimage = {
                    "schema_id": "axissim.gemm-pair-study.row",
                    "schema_version": 2,
                    "manifest_id": manifest_id,
                    "comparison_group_id": group["group_id"],
                    "source_row_index": index,
                    "request_sha256": req_sha,
                }
                row_id = canonical_hash(row_preimage)
                duplicate_of = first_for_request.get(req_bytes)
                if duplicate_of is None:
                    first_for_request[req_bytes] = index
                rows.append({
                    "study_schema_version": 2,
                    "manifest_id": manifest_id,
                    "result_payload_cap": manifest["limits"]["max_result_payload_bytes"],
                    "study_row_id": row_id,
                    "comparison_group_id": group["group_id"],
                    "source_row_index": index,
                    "shape_id": shape["shape_id"],
                    "capacity_level_id": level_id,
                    "base_case_id": f"{shape['shape_id']}_{level_id}",
                    "sensitivity_case_id": sensitivity["sensitivity_case_id"],
                    "request_sha256": req_sha,
                    "candidate_pair_id": ids["candidate_pair_id"],
                    "duplicate_of": duplicate_of,
                    "request": req,
                    "request_bytes": request_file,
                    "ids": ids,
                    "request_path": f"requests/{ids['candidate_pair_id']}.request.json",
                    "result_path": f"results/{ids['candidate_pair_id']}.result.json",
                })
    unique = {row["request_bytes"] for row in rows}
    limits = manifest["limits"]
    if len(rows) != 24 or len(unique) != 24:
        _fail("candidate_limit", "M6.3d must expand to exactly 24 unique paired requests")
    if len(rows) > limits["max_source_rows"] or len(unique) > limits["max_unique_candidates"]:
        _fail("candidate_limit", "manifest expansion exceeds the fixed M6.3d candidate limits")
    return rows


def _plan_row(row: dict[str, Any]) -> dict[str, Any]:
    result = {
        "study_row_id": row["study_row_id"],
        "comparison_group_id": row["comparison_group_id"],
        "source_row_index": row["source_row_index"],
        "shape_id": row["shape_id"],
        "capacity_level_id": row["capacity_level_id"],
        "request_sha256": row["request_sha256"],
        "candidate_pair_id": row["candidate_pair_id"],
        "request_path": row["request_path"],
        "result_path": row["result_path"],
    }
    if row["duplicate_of"] is not None:
        result["duplicate_of"] = row["duplicate_of"]
    if row.get("study_schema_version") == 2:
        result["base_case_id"] = row["base_case_id"]
        result["sensitivity_case_id"] = row["sensitivity_case_id"]
    return result


def make_plan(manifest: dict[str, Any], manifest_id: str, rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schema_id": "axissim.gemm-pair-study.plan",
        "schema_version": manifest["schema_version"],
        "manifest_id": manifest_id,
        "study_label": manifest["study_label"],
        "source_row_count": len(rows),
        "unique_candidate_count": len({r["request_bytes"] for r in rows}),
        "rows": [_plan_row(row) for row in rows],
    }


def _safe_rel(rel: str) -> PurePosixPath:
    if not isinstance(rel, str) or not rel.isascii():
        _fail("unsafe_path", "artifact path must be ASCII text", exit_code=4)
    path = PurePosixPath(rel)
    if path.is_absolute() or not path.parts or any(part in ("", ".", "..") for part in path.parts):
        _fail("unsafe_path", "artifact path is not a safe relative POSIX path", exit_code=4)
    if "\\" in rel or any("\x00" in part for part in path.parts):
        _fail("unsafe_path", "artifact path contains an unsafe character", exit_code=4)
    return path


def _check_no_symlink(root: Path, relative: str | None = None) -> Path:
    if root.is_symlink():
        _fail("unsafe_path", "output root must not be a symlink", exit_code=4)
    resolved_root = root.resolve(strict=True)
    if relative is None:
        return resolved_root
    rel = _safe_rel(relative)
    current = root
    for part in rel.parts:
        current = current / part
        if current.is_symlink():
            _fail("unsafe_path", f"symlink component in artifact path: {relative}", exit_code=4)
    resolved = current.resolve(strict=False)
    if not resolved.is_relative_to(resolved_root):
        _fail("unsafe_path", f"artifact escapes output root: {relative}", exit_code=4)
    return current


def file_bytes_under(root: Path) -> int:
    total = 0
    for path in root.rglob("*"):
        if path.is_symlink():
            _fail("unsafe_path", "output bundle contains a symlink", exit_code=4)
        if path.is_file():
            total += path.stat().st_size
    return total


class ArtifactStore:
    def __init__(self, root: Path):
        self.root = root.resolve(strict=True)
        if root.is_symlink():
            _fail("unsafe_path", "output root must not be a symlink", exit_code=4)
        (self.root / ".staging").mkdir(exist_ok=True)

    def write(self, rel: str, data: bytes, *, cap: int | None = None, bundle_cap: int = FIXED_LIMITS["max_bundle_bytes"]) -> None:
        if cap is not None and len(data) > cap:
            _fail("output_limit", f"artifact {rel} exceeds its {cap}-byte cap", exit_code=5)
        dest = _check_no_symlink(self.root, rel)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.parent.is_symlink():
            _fail("unsafe_path", f"symlink parent for {rel}", exit_code=4)
        current_size = file_bytes_under(self.root)
        staged_size = current_size + len(data)
        if staged_size > bundle_cap:
            _fail("output_limit", "atomic staging write would exceed the 2 MiB bundle cap", exit_code=5)
        temp_name = hashlib.sha256(rel.encode("ascii")).hexdigest() + ".tmp"
        temp = _check_no_symlink(self.root, f".staging/{temp_name}")
        try:
            with temp.open("wb") as handle:
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            if file_bytes_under(self.root) > bundle_cap:
                _fail("output_limit", "staged artifact exceeds the 2 MiB bundle cap", exit_code=5)
            os.replace(temp, dest)
        except RunnerError:
            raise
        except OSError as exc:
            _fail("io_error", f"cannot atomically write {rel}: {exc}", exit_code=4)
        if file_bytes_under(self.root) > bundle_cap:
            _fail("output_limit", "final artifact exceeds the 2 MiB bundle cap", exit_code=5)

    def write_json(self, rel: str, value: Any, *, cap: int | None = None) -> None:
        self.write(rel, canonical_bytes(value) + b"\n", cap=cap)

    def write_jsonl(self, rel: str, rows: list[dict[str, Any]], *, cap: int | None = None, row_cap: int | None = None) -> None:
        lines = []
        for row in rows:
            line = canonical_bytes(row) + b"\n"
            if row_cap is not None and len(line) > row_cap:
                _fail("output_limit", f"JSONL row in {rel} exceeds {row_cap} bytes", exit_code=5)
            lines.append(line)
        self.write(rel, b"".join(lines), cap=cap)


def _output_root(path_text: str, *, must_not_exist: bool) -> Path:
    candidate = Path(path_text).expanduser()
    if must_not_exist and candidate.exists():
        _fail("output_exists", "output directory already exists; choose a new empty path", exit_code=4)
    parent = candidate.parent
    try:
        parent.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        _fail("io_error", f"cannot create output parent: {exc}", exit_code=4)
    if candidate.exists() and (candidate.is_symlink() or not candidate.is_dir()):
        _fail("unsafe_path", "output root must be a new directory and not a symlink", exit_code=4)
    if must_not_exist:
        try:
            candidate.mkdir()
        except OSError as exc:
            _fail("io_error", f"cannot create output root: {exc}", exit_code=4)
    if candidate.is_symlink():
        _fail("unsafe_path", "output root must not be a symlink", exit_code=4)
    return candidate.resolve(strict=True)


def _expected_plan_paths(rows: list[dict[str, Any]]) -> set[str]:
    paths = {"manifest.json", "plan.json"}
    for row in rows:
        paths.add(row["request_path"])
    return paths


def _actual_file_paths(root: Path) -> set[str]:
    result: set[str] = set()
    for item in root.rglob("*"):
        if item.is_symlink():
            _fail("unsafe_path", "bundle contains a symlink", exit_code=4)
        if item.is_file() and item.relative_to(root).as_posix() != ".staging":
            result.add(item.relative_to(root).as_posix())
    return result


def _ensure_expected_files(root: Path, expected: set[str]) -> None:
    actual = _actual_file_paths(root)
    if actual != expected:
        _fail("provenance_failed", f"unexpected or missing phase files: extra={sorted(actual-expected)}, missing={sorted(expected-actual)}", exit_code=4)
    staging = root / ".staging"
    if staging.exists() and any(staging.iterdir()):
        _fail("integrity_failed", "staging directory is not empty", exit_code=4)


def _checkout_context(source_root: Path) -> tuple[str, dict[str, str]]:
    try:
        head = subprocess.check_output(["git", "-C", str(source_root), "rev-parse", "HEAD"], stderr=subprocess.DEVNULL, timeout=5).decode("ascii").strip()
        status = subprocess.check_output(["git", "-C", str(source_root), "status", "--porcelain", "--untracked-files=no"], stderr=subprocess.DEVNULL, timeout=5).decode("ascii")
        if status:
            diff = subprocess.check_output(["git", "-C", str(source_root), "diff", "--binary", "HEAD"], stderr=subprocess.DEVNULL, timeout=10)
            tree_state = "dirty"
            diff_hash = sha256_bytes(diff)
        else:
            tree_state = "clean"
            diff_hash = "not_applicable"
        context = {
            "schema_id": STUDY_SOURCE_ID,
            "schema_version": 1,
            "head": head,
            "tracked_tree_state": tree_state,
            "dirty_diff_sha256": diff_hash,
        }
        return canonical_hash(context), context
    except (OSError, subprocess.SubprocessError, UnicodeDecodeError):
        return "unknown", {
            "schema_id": STUDY_SOURCE_ID,
            "schema_version": 1,
            "head": "unknown",
            "tracked_tree_state": "unknown",
            "dirty_diff_sha256": "unknown",
        }


def _file_hash(path: Path) -> str:
    h = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                h.update(chunk)
    except OSError as exc:
        _fail("provenance_failed", f"cannot hash {path.name}: {exc}", exit_code=4)
    return h.hexdigest()


def build_provenance(binary: Path, script_path: Path, source_root: Path, manifest_id: str, rows: list[dict[str, Any]]) -> dict[str, Any]:
    if not binary.is_file():
        _fail("provenance_failed", "evaluator binary is missing", exit_code=4)
    runner_sha = _file_hash(script_path)
    exe_sha = _file_hash(binary)
    checkout_id, context = _checkout_context(source_root)
    if checkout_id == "unknown":
        _fail("provenance_failed", "source checkout identity could not be established", exit_code=4)
    request_hashes = [row["request_sha256"] for row in rows]
    run_preimage = {
        "schema_id": STUDY_ID_SCHEMA,
        "schema_version": 1,
        "manifest_id": manifest_id,
        "runner_source_sha256": runner_sha,
        "executable_sha256": exe_sha,
        "source_checkout_id": checkout_id,
        "ordered_request_sha256s": request_hashes,
    }
    return {
        "manifest_id": manifest_id,
        "runner_source_sha256": runner_sha,
        "executable_sha256": exe_sha,
        "source_checkout_id": checkout_id,
        "source_revision": context["head"],
        "source_tree_state": context["tracked_tree_state"],
        "dirty_diff_sha256": context["dirty_diff_sha256"],
        "study_run_id": canonical_hash(run_preimage),
        "source_context": context,
        "request_sha256s": request_hashes,
    }


def _safe_binary(path_text: str) -> Path:
    path = Path(path_text).expanduser().resolve(strict=True)
    if not path.is_file():
        _fail("provenance_failed", "evaluator binary is not a regular file", exit_code=4)
    return path


def _compute_storage_bound(root: Path, rows: list[dict[str, Any]], source_row_count: int) -> tuple[int, int, int]:
    is_v2 = bool(rows and rows[0].get("study_schema_version") == 2)
    if is_v2:
        caps = V2_FILE_CAPS
        limits = V2_LIMITS
        manifest_bytes = (root / "manifest.json").stat().st_size
        request_sizes = sum((root / row["request_path"]).stat().st_size for row in rows if row["duplicate_of"] is None)
        unique = len({row["request_bytes"] for row in rows})
        plot_reserve = caps["plots/compute-cycles.svg"] + caps["plots/logical-payload-bytes.svg"] + caps["plots/serialized-total-cycles.svg"] + caps["plots/serialized-cycle-breakdown.svg"]
        b_final = (
            manifest_bytes + caps["plan.json"] + caps["preflight.json"] + caps["provenance.json"]
            + request_sizes + unique * (limits["max_result_payload_bytes"] + 1) + source_row_count * 2048
            + caps["summary.json"] + caps["summary.csv"] + caps["report.md"] + plot_reserve
            + caps["canonical-artifact-manifest.json"] + (unique + 2) * limits["max_diagnostics_bytes_per_invocation"]
            + caps["bundle-index.json"]
        )
        peak = b_final + MAX_STAGING
        return request_sizes, b_final, peak
    manifest_bytes = (root / "manifest.json").stat().st_size
    request_sizes = sum((root / row["request_path"]).stat().st_size for row in rows if row["duplicate_of"] is None)
    unique = len({row["request_bytes"] for row in rows})
    b_final = (
        manifest_bytes + FILE_CAPS["plan.json"] + FILE_CAPS["preflight.json"] + FILE_CAPS["provenance.json"]
        + request_sizes + unique * 65537 + source_row_count * 2048
        + FILE_CAPS["summary.json"] + FILE_CAPS["summary.csv"] + FILE_CAPS["report.md"]
        + 2 * 65536 + FILE_CAPS["canonical-artifact-manifest.json"]
        + (unique + 2) * 4096 + FILE_CAPS["bundle-index.json"]
    )
    peak = b_final + MAX_STAGING
    return request_sizes, b_final, peak


class PipeDigest:
    def __init__(self) -> None:
        self.count = 0
        self.digest = hashlib.sha256()

    def consume(self, pipe: Any) -> None:
        try:
            while True:
                try:
                    data = pipe.read(2048)
                except (OSError, ValueError):
                    break
                if not data:
                    break
                self.count += len(data)
                self.digest.update(data)
        finally:
            try:
                pipe.close()
            except (OSError, ValueError):
                pass

    def hexdigest(self) -> str:
        return self.digest.hexdigest()


def invoke_evaluator(binary: Path, request_path: Path, result_path: Path, timeout_seconds: float) -> dict[str, Any]:
    argv = [str(binary), "study-gemm-pair", "--request", str(request_path), "--result", str(result_path)]
    kwargs: dict[str, Any] = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE, "shell": False}
    if os.name == "posix":
        kwargs["start_new_session"] = True
    elif os.name == "nt":
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    start = time.monotonic_ns()
    try:
        child = subprocess.Popen(argv, **kwargs)
    except OSError as exc:
        return {
            "process_exit_code": None, "timed_out": False, "launch_error": True,
            "elapsed_ns": time.monotonic_ns() - start, "error": str(exc),
            "stdout_bytes": 0, "stdout_sha256": sha256_bytes(b""),
            "stderr_bytes": 0, "stderr_sha256": sha256_bytes(b""),
        }
    out = PipeDigest()
    err = PipeDigest()
    threads = [
        threading.Thread(target=out.consume, args=(child.stdout,), daemon=True),
        threading.Thread(target=err.consume, args=(child.stderr,), daemon=True),
    ]
    for thread in threads:
        thread.start()
    timed_out = False
    cancelled = False
    try:
        code = child.wait(timeout=max(0.0, timeout_seconds))
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            if os.name == "posix":
                os.killpg(child.pid, signal.SIGTERM)
            else:
                child.terminate()
            child.wait(timeout=0.5)
        except (OSError, subprocess.TimeoutExpired):
            try:
                if os.name == "posix":
                    os.killpg(child.pid, signal.SIGKILL)
                else:
                    child.kill()
            except OSError:
                pass
            child.wait()
        code = child.returncode
    except KeyboardInterrupt:
        cancelled = True
        try:
            if os.name == "posix":
                os.killpg(child.pid, signal.SIGTERM)
            else:
                child.terminate()
            child.wait(timeout=0.5)
        except (OSError, subprocess.TimeoutExpired):
            try:
                if os.name == "posix":
                    os.killpg(child.pid, signal.SIGKILL)
                else:
                    child.kill()
            except OSError:
                pass
            child.wait()
        code = child.returncode
    if code is not None and code < 0:
        # Python reports POSIX signal termination as a negative value. The
        # canonical study records use the conventional nonnegative status.
        code = 128 + abs(code)
    for thread in threads:
        thread.join(timeout=2)
    return {
        "process_exit_code": code,
        "timed_out": timed_out,
        "cancelled": cancelled,
        "launch_error": False,
        "elapsed_ns": time.monotonic_ns() - start,
        "stdout_bytes": out.count,
        "stdout_sha256": out.hexdigest(),
        "stderr_bytes": err.count,
        "stderr_sha256": err.hexdigest(),
    }


def _cpp_response(result_path: Path, payload_cap: int | None = None) -> tuple[Any | None, bytes | None, str | None]:
    cap = FIXED_LIMITS["max_result_payload_bytes"] if payload_cap is None else payload_cap
    try:
        if result_path.stat().st_size > cap + 1:
            return None, None, "result_output_limit"
        with result_path.open("rb") as handle:
            raw = handle.read(cap + 2)
    except FileNotFoundError:
        return None, None, None
    except OSError as exc:
        return None, None, str(exc)
    if len(raw) > cap + 1:
        return None, None, "result_output_limit"
    if len(raw) < 1 or not raw.endswith(b"\n"):
        return None, raw, "result_file_size_or_newline"
    payload = raw[:-1]
    if len(payload) > cap:
        return None, raw, "result_payload_limit"
    try:
        return parse_json(payload, max_bytes=cap), raw, None
    except RunnerError:
        return None, raw, "result_json_invalid"


def _error_codes(record: Any) -> list[str]:
    if not isinstance(record, dict):
        return ["result_schema_error"]
    errors = record.get("errors", [])
    out: list[str] = []
    if isinstance(errors, list):
        for item in errors[:4]:
            if isinstance(item, dict) and isinstance(item.get("code"), str):
                code = item["code"]
                if not code.isascii() or len(code.encode("ascii")) > 64:
                    return ["error_code_limit"]
                path = item.get("path")
                if isinstance(path, str) and (not path.isascii() or len(path.encode("ascii", errors="ignore")) > 256):
                    return ["error_path_limit"]
                out.append(code)
    return out[:4]


def _error_paths(record: Any) -> list[str]:
    if not isinstance(record, dict):
        return []
    errors = record.get("errors", [])
    out: list[str] = []
    if isinstance(errors, list):
        for item in errors[:4]:
            if isinstance(item, dict) and isinstance(item.get("path"), str):
                path = item["path"]
                if not path.isascii() or len(path.encode("ascii")) > 256:
                    return [""]
                out.append(path)
    return out[:4]


def validate_candidate_result(record: Any, row: dict[str, Any], binary_sha: str) -> tuple[bool, str]:
    is_v2 = row.get("study_schema_version") == 2
    top_keys = {
        "schema_id", "schema_version", "canonicalization_id", "candidate_pair_id", "request_sha256",
        "model_assumptions_sha256", "simulator", "workload_id", "measurement_protocol", "repeat_count",
        "status", "arms", "model_coverage", "errors", "energy_status", "power_status", "budget_status",
        "physical_feasibility", "uncertainty_class",
    }
    if (not isinstance(record, dict) or set(record) != top_keys or
            record.get("schema_id") != RESULT_SCHEMA or
            isinstance(record.get("schema_version"), bool) or
            not isinstance(record.get("schema_version"), int) or
            record.get("schema_version") != 1):
        return False, "result_schema_error"
    ids = row["ids"]
    for key, expected in {
        "candidate_pair_id": ids["candidate_pair_id"], "request_sha256": ids["request_sha256"],
        "model_assumptions_sha256": ids["model_assumptions_sha256"], "workload_id": ids["workload_id"],
    }.items():
        if record.get(key) != expected:
            return False, "identity_mismatch"
    if record.get("canonicalization_id") != CANONICALIZATION:
        return False, "result_schema_error"
    simulator = record.get("simulator")
    simulator_keys = {"source_revision", "source_tree_state", "executable_sha256", "model_semantics_id", "compiler_id", "compiler_version", "build_configuration_id", "consumed_inputs"}
    if not isinstance(simulator, dict) or set(simulator) != simulator_keys or simulator.get("executable_sha256") != binary_sha:
        return False, "identity_mismatch"
    if any(not isinstance(simulator.get(key), str) for key in simulator_keys - {"consumed_inputs"}) or simulator.get("model_semantics_id") != "axissim.rvv_row_v1+axissim.tiled_matrix_output_stationary_v1" or simulator.get("consumed_inputs") != []:
        return False, "result_schema_error"
    arms = record.get("arms")
    if not isinstance(arms, dict) or set(arms) != {"rvv", "matrix"} or not all(isinstance(arms.get(n), dict) for n in ("rvv", "matrix")):
        return False, "result_schema_error"
    if arms["rvv"].get("mapping_id") not in {None, ids["rvv_mapping_id"]} or arms["matrix"].get("mapping_id") not in {None, ids["matrix_mapping_id"]}:
        return False, "identity_mismatch"
    expected_measurement = {
        "protocol": "finite_serial_batch_v1", "release_policy": "all_at_zero", "unit_order": "ascending_index_no_overlap",
        "clock_hz": 1_000_000_000, "primary_timing_scope": "compute_only", "primary_completion_boundary": "last_unit_compute",
        "overlap_policy": "none", "p99_method": "nearest_rank_one_based",
    }
    if is_v2:
        expected_measurement["backing_store_sensitivity"] = row["request"]["measurement"]["backing_store_sensitivity"]
    if (canonical_bytes(record.get("measurement_protocol")) != canonical_bytes(expected_measurement) or
            isinstance(record.get("repeat_count"), bool) or record.get("repeat_count") != 1):
        return False, "result_schema_error"
    status = record.get("status")
    if status not in {"complete", "partial", "failed"}:
        return False, "result_schema_error"
    errors = record.get("errors")
    if not isinstance(errors, list):
        return False, "result_schema_error"
    for error in errors:
        if not isinstance(error, dict) or set(error) != {"scope", "code", "path"} or not all(isinstance(error.get(k), str) for k in ("scope", "code", "path")):
            return False, "result_schema_error"
        if not error["code"].isascii() or not error["path"].isascii() or len(error["code"]) > 64 or len(error["path"].encode("ascii")) > 256:
            return False, "result_schema_error"
    if status == "complete" and errors:
        return False, "result_schema_error"
    complete_count = 0
    base_arm_keys = {
        "arm_status", "mapping_id", "mapping_kind", "workload_counts", "logical_traffic", "compute_cycles_per_unit",
        "compute_cycles_total", "compute_duration_seconds", "finite_compute_makespan_seconds", "finite_batch_rate",
        "p99_compute_completion", "modeled_mac_slot_utilization",
    }
    arm_specs = (
        ("rvv", ids["rvv_mapping_id"], "rvv_row_v1", "vector_mac_instructions"),
        ("matrix", ids["matrix_mapping_id"], "tiled_matrix_output_stationary_v1", "matrix_tile_operations"),
    )
    def uint(value: Any, *, positive: bool = False) -> bool:
        return isinstance(value, int) and not isinstance(value, bool) and (0 < value <= MAX_SAFE if positive else 0 <= value <= MAX_SAFE)

    def integer_record(value: Any, keys: set[str], *, positive: set[str] = frozenset()) -> bool:
        return isinstance(value, dict) and set(value) == keys and all(uint(value[key], positive=key in positive) for key in keys)

    for name, mapping_id, mapping_kind, operations in arm_specs:
        arm = arms[name]
        arm_status = arm.get("arm_status")
        if arm_status == "complete":
            complete_count += 1
            expected_keys = base_arm_keys | {operations + "_per_unit", operations + "_total"}
            if name == "matrix":
                expected_keys |= {"working_set_bytes", "capacity_fit"}
            if is_v2:
                expected_keys.add("serialized_backing_store")
            if set(arm) != expected_keys or arm.get("mapping_id") != mapping_id or arm.get("mapping_kind") != mapping_kind:
                return False, "result_schema_error"
            for field in ("compute_cycles_per_unit", "compute_cycles_total"):
                value = arm.get(field)
                if not uint(value):
                    return False, "result_schema_error"
            traffic_keys = {
                "read_a_bytes_per_unit", "read_a_bytes_total", "read_b_bytes_per_unit", "read_b_bytes_total",
                "write_c_bytes_per_unit", "write_c_bytes_total", "total_bytes_per_unit", "total_bytes_total",
                "read_a_batches_per_unit", "read_a_batches_total", "read_b_batches_per_unit", "read_b_batches_total",
                "write_c_batches_per_unit", "write_c_batches_total",
            }
            if not integer_record(arm.get("logical_traffic"), traffic_keys):
                return False, "result_schema_error"
            if not integer_record(arm.get("workload_counts"), {
                "useful_macs_per_unit", "useful_macs_total", "conventional_ops_per_unit", "conventional_ops_total",
            }):
                return False, "result_schema_error"
            if not integer_record(arm.get("compute_duration_seconds"), {"numerator_cycles", "denominator_hz"}, positive={"denominator_hz"}):
                return False, "result_schema_error"
            if not integer_record(arm.get("finite_compute_makespan_seconds"), {"numerator_cycles", "denominator_hz"}, positive={"denominator_hz"}):
                return False, "result_schema_error"
            if not integer_record(arm.get("finite_batch_rate"), {"numerator_hz", "denominator_cycles_per_unit"}, positive={"numerator_hz", "denominator_cycles_per_unit"}):
                return False, "result_schema_error"
            if not integer_record(arm.get("p99_compute_completion"), {"numerator_cycles", "denominator_hz", "p99_rank"}, positive={"denominator_hz", "p99_rank"}):
                return False, "result_schema_error"
            if not integer_record(arm.get("modeled_mac_slot_utilization"), {"available_mac_slots", "useful_macs"}):
                return False, "result_schema_error"
            if not uint(arm.get(operations + "_per_unit")) or not uint(arm.get(operations + "_total")):
                return False, "result_schema_error"
            if name == "matrix" and (not uint(arm.get("working_set_bytes")) or not isinstance(arm.get("capacity_fit"), bool)):
                return False, "result_schema_error"
            if is_v2:
                serialized = arm.get("serialized_backing_store")
                serialized_keys = {
                    "read_cycles_per_unit", "compute_cycles_per_unit", "write_cycles_per_unit",
                    "total_cycles_per_unit", "finite_makespan_cycles", "finite_batch_rate", "p99_completion",
                }
                if not isinstance(serialized, dict) or set(serialized) != serialized_keys:
                    return False, "result_schema_error"
                for field in (
                    "read_cycles_per_unit", "compute_cycles_per_unit", "write_cycles_per_unit",
                    "total_cycles_per_unit", "finite_makespan_cycles",
                ):
                    if not uint(serialized.get(field)):
                        return False, "result_schema_error"
                if not integer_record(serialized.get("finite_batch_rate"), {"numerator_hz", "denominator_cycles_per_unit"}, positive={"numerator_hz", "denominator_cycles_per_unit"}):
                    return False, "result_schema_error"
                if not integer_record(serialized.get("p99_completion"), {"numerator_cycles", "denominator_hz", "p99_rank"}, positive={"denominator_hz", "p99_rank"}):
                    return False, "result_schema_error"
                if serialized["compute_cycles_per_unit"] != arm["compute_cycles_per_unit"]:
                    return False, "result_schema_error"
                if serialized["total_cycles_per_unit"] != serialized["read_cycles_per_unit"] + serialized["compute_cycles_per_unit"] + serialized["write_cycles_per_unit"]:
                    return False, "result_schema_error"
                if serialized["finite_makespan_cycles"] != serialized["total_cycles_per_unit"] * record["repeat_count"]:
                    return False, "result_schema_error"
                if serialized["finite_batch_rate"] != {
                    "numerator_hz": expected_measurement["clock_hz"],
                    "denominator_cycles_per_unit": serialized["total_cycles_per_unit"],
                }:
                    return False, "result_schema_error"
                if serialized["p99_completion"] != {
                    "numerator_cycles": serialized["total_cycles_per_unit"],
                    "denominator_hz": expected_measurement["clock_hz"],
                    "p99_rank": 1,
                }:
                    return False, "result_schema_error"
        elif set(arm) != {"arm_status"} or arm_status not in {"invalid", "failed", "not_run"}:
            return False, "result_schema_error"
    if (status == "complete" and complete_count != 2) or (status == "partial" and complete_count != 1) or (status == "failed" and complete_count != 0):
        return False, "result_schema_error"
    coverage = record.get("model_coverage")
    coverage_keys = {"compute_timing", "logical_traffic", "physical_memory_actions", "memory_service", "scheduling", "host_dispatch", "overlap", "energy", "power", "area", "AME_semantics"}
    expected_coverage = {
        "compute_timing": "analytical_assumption",
        "logical_traffic": "mapping_derived_payload_only",
        "physical_memory_actions": "not_modeled",
        "memory_service": "serialized_sensitivity_only" if is_v2 else "not_requested",
        "scheduling": "serial_batch_no_arbitration",
        "host_dispatch": "excluded",
        "overlap": "excluded",
        "energy": "not_modeled",
        "power": "not_modeled",
        "area": "not_modeled",
        "AME_semantics": "not_implemented",
    }
    if not isinstance(coverage, dict) or set(coverage) != coverage_keys or coverage != expected_coverage:
        return False, "result_schema_error"
    if record.get("energy_status") != "not_modeled" or record.get("power_status") != "not_modeled" or record.get("budget_status") != "not_evaluated" or record.get("physical_feasibility") != "not_evaluated" or record.get("uncertainty_class") != "assumption_limited_deterministic_model":
        return False, "result_schema_error"
    if status == "complete":
        rvv_counts = arms["rvv"]["workload_counts"]
        matrix_counts = arms["matrix"]["workload_counts"]
        if rvv_counts != matrix_counts or arms["matrix"]["capacity_fit"] is not True:
            return False, "result_schema_error"
        for name in ("rvv", "matrix"):
            arm = arms[name]
            traffic = arm["logical_traffic"]
            counts = arm["workload_counts"]
            repeat_count = record["repeat_count"]
            if arm["compute_cycles_total"] != arm["compute_cycles_per_unit"] * repeat_count:
                return False, "result_schema_error"
            if arm["compute_duration_seconds"] != {"numerator_cycles": arm["compute_cycles_per_unit"], "denominator_hz": expected_measurement["clock_hz"]}:
                return False, "result_schema_error"
            if arm["finite_compute_makespan_seconds"] != {"numerator_cycles": arm["compute_cycles_total"], "denominator_hz": expected_measurement["clock_hz"]}:
                return False, "result_schema_error"
            if arm["finite_batch_rate"] != {"numerator_hz": expected_measurement["clock_hz"], "denominator_cycles_per_unit": arm["compute_cycles_per_unit"]}:
                return False, "result_schema_error"
            if arm["p99_compute_completion"] != {
                "numerator_cycles": arm["compute_cycles_per_unit"],
                "denominator_hz": expected_measurement["clock_hz"],
                "p99_rank": 1,
            }:
                return False, "result_schema_error"
            for stem in ("read_a_bytes", "read_b_bytes", "write_c_bytes", "total_bytes", "read_a_batches", "read_b_batches", "write_c_batches"):
                if traffic[f"{stem}_total"] != traffic[f"{stem}_per_unit"] * repeat_count:
                    return False, "result_schema_error"
            for stem in ("useful_macs", "conventional_ops"):
                if counts[f"{stem}_total"] != counts[f"{stem}_per_unit"] * repeat_count:
                    return False, "result_schema_error"
    return True, ""


def classify_cpp_response(response: Any | None, raw: bytes | None, parse_error: str | None,
                          proc: dict[str, Any], row: dict[str, Any], binary_sha: str) -> dict[str, Any]:
    code = proc.get("process_exit_code")
    result: dict[str, Any] = {
        "runner_status": "process_error",
        "process_exit_code": code,
        "error_codes": [],
        "error_paths": [],
        "cpp_status": None,
        "rvv_arm_status": None,
        "matrix_arm_status": None,
        "candidate_pair_id": None,
        "result_relative_path": None,
        "record": response,
        "raw": raw,
    }
    if proc.get("launch_error"):
        result["runner_status"] = "process_error"
        result["error_codes"] = ["process_launch_failed"]
        result["record"] = None
        result["raw"] = None
        return result
    if proc.get("timed_out"):
        result["runner_status"] = "timeout"
        result["error_codes"] = ["evaluator_timeout"]
        result["record"] = None
        result["raw"] = None
        return result
    if proc.get("cancelled"):
        result["runner_status"] = "timeout"
        result["error_codes"] = ["evaluator_cancelled"]
        result["record"] = None
        result["raw"] = None
        return result
    if parse_error == "result_output_limit":
        result["runner_status"] = "output_limit"
        result["error_codes"] = ["result_output_limit"]
        return result
    if response is None:
        if raw is not None:
            result["runner_status"] = "result_schema_error"
            result["error_codes"] = [parse_error or "result_schema_error"]
        elif code == 2:
            result["runner_status"] = "process_error"
            result["error_codes"] = ["cli_usage_no_result"]
        else:
            result["runner_status"] = "missing_result"
            result["error_codes"] = ["missing_result"]
        return result
    if not isinstance(response, dict):
        result["runner_status"] = "result_schema_error"
        result["error_codes"] = ["result_schema_error"]
        return result
    status = response.get("status")
    result["cpp_status"] = status if isinstance(status, str) else None
    result["error_codes"] = _error_codes(response)
    result["error_paths"] = _error_paths(response)
    if response.get("schema_id") == "axissim.gemm-pair.error":
        if not _has_schema_v1(response, "axissim.gemm-pair.error") or not isinstance(response.get("errors"), list):
            result["runner_status"] = "result_schema_error"
            return result
        scope = response["errors"][0].get("scope") if response["errors"] and isinstance(response["errors"][0], dict) else None
        result["result_relative_path"] = f"results/{row['study_row_id']}.error.json"
        if status in {"invalid_json", "input_limit", "unsupported_schema", "invalid_request"} and scope == "request" and code == 2:
            result["runner_status"] = "invalid"
        elif status == "internal_error" and scope == "global" and code == 4:
            result["runner_status"] = "failed"
        elif status == "output_limit" and code == 5:
            result["runner_status"] = "output_limit"
            if response.get("candidate_pair_id") == row["candidate_pair_id"]:
                result["candidate_pair_id"] = row["candidate_pair_id"]
                result["result_relative_path"] = row["result_path"]
        else:
            result["runner_status"] = "process_error"
            result["error_codes"] = ["cpp_status_exit_mismatch"]
        return result
    if not _has_schema_v1(response, RESULT_SCHEMA):
        result["runner_status"] = "result_schema_error"
        result["result_relative_path"] = row["result_path"]
        return result
    result["candidate_pair_id"] = response.get("candidate_pair_id") if isinstance(response.get("candidate_pair_id"), str) else None
    result["rvv_arm_status"] = response.get("arms", {}).get("rvv", {}).get("arm_status") if isinstance(response.get("arms"), dict) and isinstance(response.get("arms", {}).get("rvv"), dict) else None
    result["matrix_arm_status"] = response.get("arms", {}).get("matrix", {}).get("arm_status") if isinstance(response.get("arms"), dict) and isinstance(response.get("arms", {}).get("matrix"), dict) else None
    result["result_relative_path"] = row["result_path"]
    valid, invalid_status = validate_candidate_result(response, row, binary_sha)
    if not valid:
        result["runner_status"] = invalid_status
        if not result["error_codes"]:
            result["error_codes"] = [invalid_status]
        return result
    if status == "complete" and code == 0 and result["rvv_arm_status"] == "complete" and result["matrix_arm_status"] == "complete":
        result["runner_status"] = "complete"
    elif status == "partial" and code == 3:
        result["runner_status"] = "partial"
    elif status == "failed" and code == 4:
        result["runner_status"] = "failed"
    else:
        result["runner_status"] = "process_error"
        result["error_codes"] = ["cpp_status_exit_mismatch"]
    return result


def _ledger_record(row: dict[str, Any], status: str, *, execution_order: int | None = None,
                   process_exit: int | None = None, cpp_status: str | None = None,
                   rvv: str | None = None, matrix: str | None = None,
                   candidate_pair_id: str | None = None, error_codes: list[str] | None = None,
                   error_paths: list[str] | None = None, artifacts: list[str] | None = None) -> dict[str, Any]:
    rec: dict[str, Any] = {
        "schema_id": "axissim.gemm-pair-study.ledger-row",
        "schema_version": row.get("study_schema_version", 1),
        "study_row_id": row["study_row_id"],
        "comparison_group_id": row["comparison_group_id"],
        "source_row_index": row["source_row_index"],
        "runner_status": status,
        "error_codes": (error_codes or [])[:4],
        "error_paths": (error_paths or [])[:4],
        "artifact_paths": (artifacts or [])[:2],
    }
    if row.get("study_schema_version") == 2:
        rec["base_case_id"] = row["base_case_id"]
        rec["sensitivity_case_id"] = row["sensitivity_case_id"]
    if status not in STATUS_VALUES:
        _fail("internal_error", f"invalid runner row state {status}", exit_code=4)
    if execution_order is not None:
        rec["execution_order"] = execution_order
    if process_exit is not None:
        rec["process_exit_code"] = process_exit
    if row.get("request_sha256"):
        rec["request_sha256"] = row["request_sha256"]
    if candidate_pair_id:
        rec["candidate_pair_id"] = candidate_pair_id
    if cpp_status:
        rec["cpp_status"] = cpp_status
    if rvv:
        rec["rvv_arm_status"] = rvv
    if matrix:
        rec["matrix_arm_status"] = matrix
    return rec


def _canonical_result_path(row: dict[str, Any], status: str, classified: dict[str, Any]) -> str | None:
    rel = classified.get("result_relative_path")
    if rel:
        return rel
    if classified.get("raw") is None:
        return None
    return row["result_path"]


def _store_response(store: ArtifactStore, root: Path, row: dict[str, Any], classified: dict[str, Any]) -> list[str]:
    raw = classified.get("raw")
    if raw is None:
        return []
    rel = _canonical_result_path(row, classified["runner_status"], classified)
    if rel is None:
        return []
    cap = row.get("result_payload_cap", FIXED_LIMITS["max_result_payload_bytes"]) + 1
    store.write(rel, raw, cap=cap)
    return [rel]


def _observed_host() -> dict[str, str]:
    return {
        "python_version": platform.python_version(),
        "os_id": sys.platform,
        "architecture_id": platform.machine() or "unknown",
    }


def _phase_diag(phase: str, sequence: int, elapsed: int) -> dict[str, Any]:
    return {
        "schema_id": "axissim.gemm-pair-study.host-diagnostic",
        "schema_version": 1,
        "record_type": "phase",
        "phase": phase,
        "sequence": sequence,
        **_observed_host(),
        "phase_elapsed_ns": max(0, elapsed),
    }


def _refresh_v2_preflight_elapsed(store: "ArtifactStore", diagnostics: list[dict[str, Any]],
                                 phase_start_ns: int, sequence: int) -> int:
    elapsed_ns = max(0, time.monotonic_ns() - phase_start_ns)
    diagnostics[-1] = _phase_diag("preflight", sequence, elapsed_ns)
    store.write("host-diagnostics.jsonl", _jsonl_bytes(diagnostics), cap=V2_FILE_CAPS["host-diagnostics.jsonl"])
    return elapsed_ns


def _evaluator_diag(phase: str, sequence: int, row: dict[str, Any], proc: dict[str, Any]) -> dict[str, Any]:
    record = {
        "schema_id": "axissim.gemm-pair-study.host-diagnostic",
        "schema_version": 1,
        "record_type": "evaluator",
        "phase": phase,
        "sequence": sequence,
        **_observed_host(),
        "study_row_id": row["study_row_id"],
        "evaluator_elapsed_ns": max(0, proc.get("elapsed_ns", 0)),
        "stdout_bytes": proc.get("stdout_bytes", 0),
        "stdout_sha256": proc.get("stdout_sha256", sha256_bytes(b"")),
        "stderr_bytes": proc.get("stderr_bytes", 0),
        "stderr_sha256": proc.get("stderr_sha256", sha256_bytes(b"")),
    }
    if proc.get("process_exit_code") is not None:
        record["process_exit_code"] = proc["process_exit_code"]
    return record


def _write_preflight_ledger(store: ArtifactStore, rows: list[dict[str, Any]], pilot_results: dict[int, dict[str, Any]], failure_index: int | None, global_status: str) -> None:
    ledgers = []
    for row in rows:
        i = row["source_row_index"]
        if row["duplicate_of"] is not None:
            canonical = pilot_results.get(row["duplicate_of"])
            st = "duplicate"
            ledgers.append(_ledger_record(row, st, candidate_pair_id=row["candidate_pair_id"], error_codes=[], artifacts=[]))
            continue
        classified = pilot_results.get(i)
        if classified is not None:
            status = classified["runner_status"]
            ledgers.append(_ledger_record(
                row, status, execution_order=classified.get("execution_order"),
                process_exit=classified.get("process_exit_code"),
                cpp_status=classified.get("cpp_status"), rvv=classified.get("rvv_arm_status"),
                matrix=classified.get("matrix_arm_status"),
                candidate_pair_id=classified.get("candidate_pair_id"),
                error_codes=classified.get("error_codes"), error_paths=classified.get("error_paths"),
                artifacts=classified.get("artifact_paths"),
            ))
        elif global_status == "provenance_failed":
            ledgers.append(_ledger_record(row, "not_started_provenance_failure", candidate_pair_id=row["candidate_pair_id"], error_codes=["provenance_failed"]))
        elif global_status == "output_limit":
            ledgers.append(_ledger_record(row, "not_started_due_to_limit", candidate_pair_id=row["candidate_pair_id"], error_codes=["output_limit"]))
        elif global_status == "limit_reached":
            ledgers.append(_ledger_record(row, "not_started_due_to_limit", candidate_pair_id=row["candidate_pair_id"], error_codes=["active_time_limit"]))
        elif row.get("study_schema_version") == 2 and global_status == "pilot_failed" and row["capacity_level_id"] == "p8":
            ledgers.append(_ledger_record(row, "not_started_due_to_preflight_failure", candidate_pair_id=row["candidate_pair_id"], error_codes=["pilot_failed"]))
        elif failure_index is not None and i > failure_index:
            ledgers.append(_ledger_record(row, "not_started_due_to_preflight_failure", candidate_pair_id=row["candidate_pair_id"], error_codes=["pilot_failed"]))
        else:
            ledgers.append(_ledger_record(row, "not_started_due_to_preflight_failure", candidate_pair_id=row["candidate_pair_id"], error_codes=["pilot_not_run"]))
    caps = _caps_for_row(rows[0]) if rows else FILE_CAPS
    store.write_jsonl("runner-ledger.jsonl", ledgers, cap=caps["runner-ledger.jsonl"], row_cap=2048)


def _make_provenance(raw_manifest: bytes, manifest: dict[str, Any], identities: dict[str, Any], simulator: Any) -> dict[str, Any]:
    return {
        "schema_id": "axissim.gemm-pair-study.provenance",
        "schema_version": 1,
        "manifest_id": identities["manifest_id"],
        "study_label": manifest["study_label"],
        "study_run_id": identities["study_run_id"],
        "manifest_input_sha256": sha256_bytes(raw_manifest),
        "runner_source_sha256": identities["runner_source_sha256"],
        "source_checkout_id": identities["source_checkout_id"],
        "source_revision": identities["source_revision"],
        "source_tree_state": identities["source_tree_state"],
        "dirty_diff_sha256": identities["dirty_diff_sha256"],
        "executable_sha256": identities["executable_sha256"],
        "cpp_simulator_identity": simulator if simulator is not None else "unknown",
        "request_sha256s": identities["request_sha256s"],
    }


def _load_runner_manifest(manifest_path: Path) -> tuple[dict[str, Any], bytes, str]:
    try:
        raw = manifest_path.read_bytes()
    except OSError as exc:
        _fail("io_error", f"cannot read manifest: {exc}", exit_code=4)
    if len(raw) > FIXED_LIMITS["max_manifest_bytes"]:
        _fail("manifest_limit", "manifest exceeds 65,536 bytes")
    manifest = validate_manifest(parse_json(raw, max_bytes=FIXED_LIMITS["max_manifest_bytes"]))
    return manifest, raw, canonical_hash(manifest)


def command_plan(args: argparse.Namespace) -> int:
    manifest, raw, manifest_id = _load_runner_manifest(Path(args.manifest).expanduser())
    limits = _limits_for_manifest(manifest)
    caps = _caps_for_manifest(manifest)
    rows = expand_candidates(manifest, manifest_id)
    root = _output_root(args.output_dir, must_not_exist=True)
    store = ArtifactStore(root)
    store.write_json("manifest.json", manifest, cap=caps["manifest.json"])
    for row in rows:
        if row["duplicate_of"] is None:
            store.write(row["request_path"], row["request_bytes"], cap=limits["max_request_file_bytes"])
    plan = make_plan(manifest, manifest_id, rows)
    store.write_json("plan.json", plan, cap=caps["plan.json"])
    _ensure_expected_files(root, _expected_plan_paths(rows))
    print(f"Plan created: {len(rows)} source rows, {plan['unique_candidate_count']} unique requests")
    print(f"Manifest ID: {manifest_id}")
    return 0


def _preflight_obj(manifest_id: str, plan_hash: str, identities: dict[str, Any] | None,
                   status: str, source_count: int, unique_count: int, request_bytes: int,
                   peak_bytes: int, worst_seconds: int, pilot_ids: list[str], errors: list[str]) -> dict[str, Any]:
    result = {
        "schema_id": "axissim.gemm-pair-study.preflight",
        "schema_version": 1,
        "manifest_id": manifest_id,
        "plan_sha256": plan_hash,
        "study_run_id": identities["study_run_id"] if identities else "unknown",
        "executable_sha256": identities["executable_sha256"] if identities else "unknown",
        "status": status,
        "source_row_count": source_count,
        "unique_candidate_count": unique_count,
        "exact_request_file_bytes": request_bytes,
        "reserved_peak_bytes": peak_bytes,
        "worst_case_evaluator_seconds": worst_seconds,
        "pilot_row_ids": pilot_ids,
        "error_codes": errors[:4],
    }
    return result


def _candidate_result_artifact(store: ArtifactStore, row: dict[str, Any], classified: dict[str, Any]) -> list[str]:
    raw = classified.get("raw")
    if raw is None:
        return []
    rel = classified.get("result_relative_path")
    if rel is None:
        rel = row["result_path"] if classified.get("candidate_pair_id") else f"results/{row['study_row_id']}.error.json"
    store.write(rel, raw, cap=row.get("result_payload_cap", FIXED_LIMITS["max_result_payload_bytes"]) + 1)
    return [rel]


def _invoke_candidate(binary: Path, root: Path, row: dict[str, Any], timeout: float) -> tuple[dict[str, Any], Any | None, bytes | None, str | None]:
    """Keep evaluator output in staging until it has been parsed and classified."""
    temp_name = hashlib.sha256(("cpp-result:" + row["study_row_id"]).encode("ascii")).hexdigest() + ".json"
    temp_path = _check_no_symlink(root, ".staging/" + temp_name)
    if temp_path.exists():
        _fail("integrity_failed", "staging result path already exists", exit_code=4)
    if temp_path.is_symlink():
        _fail("unsafe_path", "staging result path is a symlink", exit_code=4)
    proc = invoke_evaluator(binary, root / row["request_path"], temp_path, timeout)
    response, raw, parse_error = _cpp_response(temp_path, row.get("result_payload_cap"))
    try:
        temp_path.unlink(missing_ok=True)
    except OSError as exc:
        _fail("io_error", f"cannot remove staged evaluator result: {exc}", exit_code=4)
    return proc, response, raw, parse_error


def _store_identityless_request(store: ArtifactStore, row: dict[str, Any], classified: dict[str, Any]) -> list[str]:
    if classified.get("candidate_pair_id") is not None or classified.get("raw") is None:
        return []
    if classified.get("runner_status") not in {"invalid", "failed", "output_limit", "result_schema_error", "identity_mismatch", "process_error"}:
        return []
    rel = f"requests/{row['study_row_id']}.request.json"
    limits = V2_LIMITS if row.get("study_schema_version") == 2 else FIXED_LIMITS
    store.write(rel, row["request_bytes"], cap=limits["max_request_file_bytes"])
    return [rel]


def _pilot_candidates(rows: list[dict[str, Any]], manifest: dict[str, Any]) -> list[dict[str, Any]]:
    if manifest.get("schema_version") == 2:
        pilots = [row for row in rows if row["capacity_level_id"] == "p4"]
        if len(pilots) != 12:
            _fail("candidate_limit", "M6.3d must have exactly twelve P4 pilot rows")
        return pilots
    by_group = {g["group_id"]: g for g in manifest["comparison_groups"]}
    pilots = []
    seen: set[bytes] = set()
    row_by_group_level = {(r["comparison_group_id"], r["capacity_level_id"]): r for r in rows}
    for group in manifest["comparison_groups"]:
        row = row_by_group_level[(group["group_id"], group["pilot_level_id"])]
        if row["request_bytes"] not in seen:
            pilots.append(row)
            seen.add(row["request_bytes"])
    return pilots


def command_preflight(args: argparse.Namespace) -> int:
    phase_start_ns = time.monotonic_ns()
    root_arg = Path(args.output_dir).expanduser()
    if root_arg.is_symlink():
        _fail("unsafe_path", "output root must not be a symlink", exit_code=4)
    root = root_arg.resolve(strict=True)
    store = ArtifactStore(root)
    manifest, raw_manifest, manifest_id = _load_runner_manifest(Path(args.manifest).expanduser())
    limits = _limits_for_manifest(manifest)
    caps = _caps_for_manifest(manifest)
    # Keep every phase input tied to the exact plan created by `plan`.
    plan_obj, regenerated_plan, rows = _read_phase_artifacts_from_manifest(manifest, manifest_id, root)
    _ensure_expected_files(root, _expected_plan_paths(rows))
    plan_hash = sha256_bytes(canonical_bytes(plan_obj))
    request_bytes, b_final, b_peak = _compute_storage_bound(root, rows, len(rows))
    unique = len({r["request_bytes"] for r in rows})
    pilots = _pilot_candidates(rows, manifest)
    pilot_ids = [row["study_row_id"] for row in pilots]
    worst_seconds = unique * limits["max_evaluator_seconds"]
    identities: dict[str, Any] | None = None
    try:
        binary = _safe_binary(args.binary)
        script_path = Path(__file__).resolve(strict=True)
        source_root = Path(__file__).resolve().parents[1]
        identity = build_provenance(binary, script_path, source_root, manifest_id, rows)
        identities = identity
    except RunnerError as exc:
        pf = _preflight_obj(manifest_id, plan_hash, None, "provenance_failed", len(rows), unique, request_bytes, b_peak, worst_seconds, pilot_ids, [exc.code])
        store.write_json("preflight.json", pf, cap=caps["preflight.json"])
        unknown_identity = {
            "manifest_id": manifest_id, "study_run_id": "unknown", "runner_source_sha256": "unknown",
            "source_checkout_id": "unknown", "source_revision": "unknown", "source_tree_state": "unknown",
            "dirty_diff_sha256": "unknown", "executable_sha256": "unknown",
            "request_sha256s": [row["request_sha256"] for row in rows],
        }
        provenance = _make_provenance(raw_manifest, manifest, unknown_identity, "unknown")
        store.write_json("provenance.json", provenance, cap=caps["provenance.json"])
        elapsed = time.monotonic_ns() - phase_start_ns
        _write_terminal_preflight_bundle(store, manifest, rows, {}, "provenance_failed", "unknown",
                                         [_phase_diag("preflight", 0, elapsed)])
        if manifest.get("schema_version") == 2:
            terminal_status = _finish_v2_terminal_preflight(store, manifest, rows, {}, "provenance_failed", "unknown",
                                                            [_phase_diag("preflight", 0, elapsed)], phase_start_ns, 0)
            return 3 if terminal_status == "limit_reached" else 4
        return 4
    if b_peak > limits["max_bundle_bytes"]:
        pf = _preflight_obj(manifest_id, plan_hash, identities, "output_limit", len(rows), unique, request_bytes, b_peak, worst_seconds, pilot_ids, ["bundle_preflight_limit"])
        store.write_json("preflight.json", pf, cap=caps["preflight.json"])
        provenance = _make_provenance(raw_manifest, manifest, identities, "unknown")
        store.write_json("provenance.json", provenance, cap=caps["provenance.json"])
        elapsed = time.monotonic_ns() - phase_start_ns
        _write_terminal_preflight_bundle(store, manifest, rows, {}, "output_limit", identities["study_run_id"],
                                         [_phase_diag("preflight", 0, elapsed)])
        if manifest.get("schema_version") == 2:
            terminal_status = _finish_v2_terminal_preflight(store, manifest, rows, {}, "output_limit", identities["study_run_id"],
                                                            [_phase_diag("preflight", 0, elapsed)], phase_start_ns, 0)
            return 3 if terminal_status == "limit_reached" else 5
        return 5
    diag: list[dict[str, Any]] = []
    pilot_records: dict[int, dict[str, Any]] = {}
    failure_index: int | None = None
    simulator_identity: Any = None
    sequence = 0
    pilot_gate_failed = False
    for row in pilots:
        elapsed_before = (time.monotonic_ns() - phase_start_ns) / 1_000_000_000
        remaining = limits["max_active_seconds"] - elapsed_before
        if remaining <= 0:
            failure_index = row["source_row_index"]
            pf_status = "limit_reached"
            break
        timeout = min(float(limits["max_evaluator_seconds"]), remaining)
        proc, response, raw, parse_error = _invoke_candidate(binary, root, row, timeout)
        diag.append(_evaluator_diag("preflight", sequence, row, proc))
        execution_order = sequence
        sequence += 1
        classified = classify_cpp_response(response, raw, parse_error, proc, row, identities["executable_sha256"])
        classified.update({"execution_order": execution_order, "process_exit_code": proc.get("process_exit_code")})
        if raw is not None:
            artifacts = _store_identityless_request(store, row, classified)
            artifacts.extend(_candidate_result_artifact(store, row, classified))
            classified["artifact_paths"] = artifacts
            if isinstance(response, dict):
                simulator_identity = response.get("simulator", simulator_identity)
        pilot_records[row["source_row_index"]] = classified
        if classified["runner_status"] != "complete":
            if failure_index is None:
                failure_index = row["source_row_index"]
            if proc.get("cancelled") or (proc.get("timed_out") and remaining <= timeout):
                pf_status = "limit_reached"
                break
            pilot_gate_failed = True
            if manifest.get("schema_version") != 2:
                pf_status = "pilot_failed"
                break
    else:
        pf_status = "pilot_failed" if pilot_gate_failed else "ready"
    elapsed_ns = time.monotonic_ns() - phase_start_ns
    diag.append(_phase_diag("preflight", sequence, elapsed_ns))
    pf = _preflight_obj(manifest_id, plan_hash, identities, pf_status, len(rows), unique, request_bytes, b_peak, worst_seconds, pilot_ids, [] if pf_status == "ready" else [pf_status])
    store.write_json("preflight.json", pf, cap=caps["preflight.json"])
    provenance = _make_provenance(raw_manifest, manifest, identities, simulator_identity)
    store.write_json("provenance.json", provenance, cap=caps["provenance.json"])
    if pf_status != "ready":
        _write_terminal_preflight_bundle(store, manifest, rows, pilot_records, pf_status,
                                         identities["study_run_id"], diag, failure_index)
        if manifest.get("schema_version") == 2:
            pf_status = _finish_v2_terminal_preflight(store, manifest, rows, pilot_records, pf_status,
                                                      identities["study_run_id"], diag, phase_start_ns,
                                                      sequence, failure_index)
        print(f"Preflight status: {pf_status}")
        return 3 if pf_status in {"pilot_failed", "limit_reached"} else (5 if pf_status == "output_limit" else 4)
    store.write("host-diagnostics.jsonl", _jsonl_bytes(diag), cap=caps["host-diagnostics.jsonl"])
    _ensure_expected_files(root, _expected_preflight_paths(rows, pilots))
    if manifest.get("schema_version") == 2:
        elapsed_ns = _refresh_v2_preflight_elapsed(store, diag, phase_start_ns, sequence)
        if elapsed_ns > limits["max_active_seconds"] * 1_000_000_000:
            pf_status = "limit_reached"
            pf = _preflight_obj(manifest_id, plan_hash, identities, pf_status, len(rows), unique, request_bytes,
                                b_peak, worst_seconds, pilot_ids, [pf_status])
            store.write_json("preflight.json", pf, cap=caps["preflight.json"])
            _write_terminal_preflight_bundle(store, manifest, rows, pilot_records, pf_status,
                                             identities["study_run_id"], diag, failure_index)
            elapsed_ns = _refresh_v2_preflight_elapsed(store, diag, phase_start_ns, sequence)
            _finalize_manifest(store)
            verify_bundle(root)
            print("Preflight status: limit_reached")
            return 3
    print(f"Preflight ready: {len(pilots)} pilot cases complete")
    print(f"Worst-case evaluator process time: {worst_seconds} s; reserved peak: {b_peak} bytes")
    return 0


def _read_phase_artifacts_from_manifest(manifest: dict[str, Any], manifest_id: str, root: Path) -> tuple[dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    try:
        saved_manifest = (root / "manifest.json").read_bytes()
    except OSError:
        _fail("provenance_failed", "canonical manifest artifact is missing", exit_code=4)
    if saved_manifest != canonical_bytes(manifest) + b"\n":
        _fail("identity_mismatch", "canonical manifest artifact differs from the supplied manifest", exit_code=4)
    plan_obj, _ = read_json(root / "plan.json", max_bytes=FILE_CAPS["plan.json"])
    rows = expand_candidates(manifest, manifest_id)
    expected_plan = make_plan(manifest, manifest_id, rows)
    if plan_obj != expected_plan:
        _fail("provenance_failed", "plan does not match the manifest", exit_code=4)
    for row in rows:
        try:
            raw = (root / row["request_path"]).read_bytes()
        except OSError:
            _fail("provenance_failed", f"planned request missing: {row['request_path']}", exit_code=4)
        if raw != row["request_bytes"]:
            _fail("identity_mismatch", "request artifact differs from regenerated canonical bytes", exit_code=4)
    return plan_obj, expected_plan, rows


def _expected_preflight_paths(rows: list[dict[str, Any]], pilots: list[dict[str, Any]]) -> set[str]:
    paths = _expected_plan_paths(rows) | {"preflight.json", "provenance.json", "host-diagnostics.jsonl"}
    for row in pilots:
        paths.add(row["result_path"])
    return paths


def _jsonl_bytes(rows: list[dict[str, Any]]) -> bytes:
    lines = [canonical_bytes(row) + b"\n" for row in rows]
    if any(len(line) > 4096 for line in lines):
        _fail("output_limit", "a JSONL record exceeds the 4,096-byte row cap", exit_code=5)
    return b"".join(lines)


def _row_map_by_index(rows: list[dict[str, Any]]) -> dict[int, dict[str, Any]]:
    return {row["source_row_index"]: row for row in rows}


def _metrics_for(record: dict[str, Any]) -> dict[str, Any]:
    arms = record["arms"]
    metrics: dict[str, Any] = {}
    for name in ("rvv", "matrix"):
        arm = arms[name]
        out = {
            "mapping_id": arm["mapping_id"],
            "mapping_kind": arm["mapping_kind"],
            "workload_counts": arm["workload_counts"],
            "compute_cycles_per_unit": arm["compute_cycles_per_unit"],
            "compute_duration_seconds": arm["compute_duration_seconds"],
            "finite_compute_makespan_seconds": arm["finite_compute_makespan_seconds"],
            "finite_batch_rate": arm["finite_batch_rate"],
            "p99_compute_completion": arm["p99_compute_completion"],
            "logical_traffic": arm["logical_traffic"],
        }
        for key in ("vector_mac_instructions_per_unit", "vector_mac_instructions_total", "matrix_tile_operations_per_unit", "matrix_tile_operations_total", "working_set_bytes", "capacity_fit"):
            if key in arm:
                out[key] = arm[key]
        if "serialized_backing_store" in arm:
            out["serialized_backing_store"] = arm["serialized_backing_store"]
        metrics[name] = out
    return metrics


def _summary_row(row: dict[str, Any], classified: dict[str, Any]) -> dict[str, Any]:
    if row.get("study_schema_version") == 2:
        return _summary_row_v2(row, classified)
    out: dict[str, Any] = {
        "study_row_id": row["study_row_id"],
        "source_row_index": row["source_row_index"],
        "comparison_group_id": row["comparison_group_id"],
        "shape_id": row["shape_id"],
        "capacity_level_id": row["capacity_level_id"],
        "runner_status": classified["runner_status"],
    }
    for key in ("candidate_pair_id", "cpp_status", "rvv_arm_status", "matrix_arm_status"):
        value = classified.get(key)
        if value is not None:
            out[key] = value
    if classified.get("error_codes"):
        out["error_codes"] = classified["error_codes"][:4]
    if classified.get("artifact_paths"):
        out["artifact_paths"] = classified["artifact_paths"][:2]
    if classified["runner_status"] == "complete" and isinstance(classified.get("record"), dict):
        out["metrics"] = _metrics_for(classified["record"])
    request = row["request"]
    out["workload_dimensions"] = {key: request["workload"][key] for key in ("m", "n", "k")}
    out["capacity_assumption"] = {
        "mac_slots_per_cycle": request["mappings"]["matrix"]["mac_slots_per_cycle"],
        "physical_cost_equivalence": "not_claimed",
    }
    out["mapping_parameters"] = {
        "rvv": dict(request["mappings"]["rvv"]),
        "matrix": dict(request["mappings"]["matrix"]),
    }
    return out


def _summary_row_v2(row: dict[str, Any], classified: dict[str, Any]) -> dict[str, Any]:
    req = row["request"]
    ids = row["ids"]
    sensitivity = req["measurement"]["backing_store_sensitivity"]
    out: dict[str, Any] = {
        "schema_id": "axissim.gemm-pair-study.summary-row",
        "schema_version": 2,
        "manifest_id": row["manifest_id"],
        "source_row_index": row["source_row_index"],
        "study_row_id": row["study_row_id"],
        "candidate_pair_id": row["candidate_pair_id"],
        "comparison_group_id": row["comparison_group_id"],
        "shape_id": row["shape_id"],
        "capacity_level_id": row["capacity_level_id"],
        "base_case_id": row["base_case_id"],
        "sensitivity_case_id": row["sensitivity_case_id"],
        "workload_id": ids["workload_id"],
        "rvv_mapping_id": ids["rvv_mapping_id"],
        "matrix_mapping_id": ids["matrix_mapping_id"],
        "request_sha256": ids["request_sha256"],
        "model_assumptions_sha256": ids["model_assumptions_sha256"],
        "runner_status": classified["runner_status"],
        "sensitivity_assumption": dict(sensitivity),
        "workload_dimensions": {key: req["workload"][key] for key in ("m", "n", "k")},
        "mapping_parameters": {key: dict(value) for key, value in req["mappings"].items()},
        "capacity_assumption": {
            "mac_slots_per_cycle": req["mappings"]["matrix"]["mac_slots_per_cycle"],
            "physical_cost_equivalence": "not_claimed",
        },
    }
    for key in ("cpp_status", "rvv_arm_status", "matrix_arm_status"):
        if classified.get(key) is not None:
            out[key] = classified[key]
    if classified.get("error_codes"):
        out["error_codes"] = classified["error_codes"][:4]
    if classified.get("artifact_paths"):
        out["artifact_paths"] = classified["artifact_paths"][:2]
    if classified["runner_status"] == "complete" and isinstance(classified.get("record"), dict):
        out["metrics"] = _metrics_for(classified["record"])
    return out


def _build_summary(manifest: dict[str, Any], rows: list[dict[str, Any]], results: dict[int, dict[str, Any]], study_status: str, study_run_id: str, *, failure_index: int | None = None) -> dict[str, Any]:
    per_row: list[dict[str, Any]] = []
    counts = {status: 0 for status in sorted(STATUS_VALUES)}
    for row in rows:
        if row["duplicate_of"] is not None:
            classified = {"runner_status": "duplicate", "candidate_pair_id": row["candidate_pair_id"], "error_codes": [], "error_paths": [], "artifact_paths": []}
        else:
            missing_status = "not_started_provenance_failure" if study_status == "provenance_failed" else "not_started_due_to_preflight_failure" if study_status == "pilot_failed" else "not_started_due_to_limit"
            if row.get("study_schema_version") == 2 and study_status == "pilot_failed":
                missing_code = "pilot_failed"
            else:
                missing_code = "provenance_failed" if study_status == "provenance_failed" else "output_limit" if study_status == "output_limit" else "active_time_limit" if study_status == "limit_reached" else "pilot_failed" if study_status == "pilot_failed" and failure_index is not None and row["source_row_index"] > failure_index else "pilot_not_run" if study_status == "pilot_failed" else missing_status
            classified = results.get(row["source_row_index"], {"runner_status": missing_status, "candidate_pair_id": row["candidate_pair_id"], "error_codes": [missing_code]})
        rec = _summary_row(row, classified)
        per_row.append(rec)
        counts[rec["runner_status"]] += 1
    summary = {
        "schema_id": "axissim.gemm-pair-study.summary",
        "schema_version": manifest["schema_version"],
        "study_label": manifest["study_label"],
        "study_run_id": study_run_id,
        "study_status": study_status,
        "counts": counts,
        "rows": per_row,
    }
    if manifest.get("schema_version") == 2:
        summary["baseline_reference"] = manifest["baseline_reference"]
        summary["sensitivity_cases"] = manifest["sensitivity_cases"]
        summary["hypotheses"] = _hypothesis_results_v2(manifest, rows, results)
        summary["interpretation_scope"] = "deterministic_model_checks_and_declared_scenario_only"
    return summary


def _summary_csv(rows: list[dict[str, Any]], results: dict[int, dict[str, Any]], summary_rows: list[dict[str, Any]] | None = None) -> str:
    output = io.StringIO(newline="")
    if rows and rows[0].get("study_schema_version") == 2:
        return _summary_csv_v2(rows, results, summary_rows)
    writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS, lineterminator="\n", extrasaction="ignore")
    writer.writeheader()
    summary_by_index = {row["source_row_index"]: row for row in (summary_rows or [])}
    for row in rows:
        classified = results.get(row["source_row_index"])
        summary_row = summary_by_index.get(row["source_row_index"])
        if row["duplicate_of"] is not None:
            status = "duplicate"
            result = None
        elif classified is None:
            status = summary_row["runner_status"] if summary_row else "not_started_due_to_limit"
            result = None
        else:
            status = classified["runner_status"]
            result = classified.get("record") if status == "complete" else None
        line = {
            "source_row_index": row["source_row_index"],
            "study_row_id": row["study_row_id"],
            "candidate_pair_id": row["candidate_pair_id"],
            "comparison_group_id": row["comparison_group_id"],
            "shape_id": row["shape_id"],
            "capacity_level_id": row["capacity_level_id"],
            "runner_status": status,
            "cpp_status": result.get("status", "") if isinstance(result, dict) else (classified.get("cpp_status", "") if classified else ""),
            "rvv_arm_status": result.get("arms", {}).get("rvv", {}).get("arm_status", "") if isinstance(result, dict) else (classified.get("rvv_arm_status", "") if classified else ""),
            "matrix_arm_status": result.get("arms", {}).get("matrix", {}).get("arm_status", "") if isinstance(result, dict) else (classified.get("matrix_arm_status", "") if classified else ""),
            "error_codes": ";".join(classified.get("error_codes", [])) if classified else ";".join(summary_row.get("error_codes", [])) if summary_row else "",
        }
        if isinstance(result, dict):
            for arm_name, prefix in (("rvv", "rvv"), ("matrix", "matrix")):
                arm = result["arms"][arm_name]
                line[f"{prefix}_compute_cycles_per_unit"] = arm["compute_cycles_per_unit"]
                traffic = arm["logical_traffic"]
                line[f"{prefix}_read_a_bytes_per_unit"] = traffic["read_a_bytes_per_unit"]
                line[f"{prefix}_read_b_bytes_per_unit"] = traffic["read_b_bytes_per_unit"]
                line[f"{prefix}_write_c_bytes_per_unit"] = traffic["write_c_bytes_per_unit"]
        writer.writerow(line)
    return output.getvalue()


def _summary_csv_v2(rows: list[dict[str, Any]], results: dict[int, dict[str, Any]], summary_rows: list[dict[str, Any]] | None) -> str:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS_V2, lineterminator="\n", extrasaction="ignore")
    writer.writeheader()
    summary_by_index = {item["source_row_index"]: item for item in (summary_rows or [])}
    for row in rows:
        classified = results.get(row["source_row_index"])
        summary_row = summary_by_index.get(row["source_row_index"], {})
        status = classified["runner_status"] if classified else summary_row.get("runner_status", "not_started_due_to_limit")
        record = classified.get("record") if classified and status == "complete" else None
        ids = row["ids"]
        sensitivity = row["request"]["measurement"]["backing_store_sensitivity"]
        line: dict[str, Any] = {
            "schema_id": "axissim.gemm-pair-study.row",
            "schema_version": 2,
            "manifest_id": row["manifest_id"],
            "source_row_index": row["source_row_index"],
            "study_row_id": row["study_row_id"],
            "candidate_pair_id": row["candidate_pair_id"],
            "comparison_group_id": row["comparison_group_id"],
            "shape_id": row["shape_id"],
            "capacity_level_id": row["capacity_level_id"],
            "base_case_id": row["base_case_id"],
            "sensitivity_case_id": row["sensitivity_case_id"],
            "workload_id": ids["workload_id"],
            "rvv_mapping_id": ids["rvv_mapping_id"],
            "matrix_mapping_id": ids["matrix_mapping_id"],
            "request_sha256": ids["request_sha256"],
            "model_assumptions_sha256": ids["model_assumptions_sha256"],
            "runner_status": status,
            "cpp_status": (record or {}).get("status", classified.get("cpp_status", "") if classified else ""),
            "rvv_arm_status": (record or {}).get("arms", {}).get("rvv", {}).get("arm_status", classified.get("rvv_arm_status", "") if classified else ""),
            "matrix_arm_status": (record or {}).get("arms", {}).get("matrix", {}).get("arm_status", classified.get("matrix_arm_status", "") if classified else ""),
            "bandwidth_bytes_per_second": sensitivity["bandwidth_bytes_per_second"],
            "batch_setup_cycles": sensitivity["batch_setup_cycles"],
            "error_codes": ";".join(classified.get("error_codes", [])) if classified else ";".join(summary_row.get("error_codes", [])),
        }
        if isinstance(record, dict):
            for arm_name in ("rvv", "matrix"):
                arm = record["arms"][arm_name]
                prefix = arm_name
                serial = arm["serialized_backing_store"]
                line[f"{prefix}_compute_cycles_per_unit"] = arm["compute_cycles_per_unit"]
                for field, csv_field in (("read_cycles_per_unit", "serialized_read_cycles_per_unit"),
                                         ("compute_cycles_per_unit", "serialized_compute_cycles_per_unit"),
                                         ("write_cycles_per_unit", "serialized_write_cycles_per_unit"),
                                         ("total_cycles_per_unit", "serialized_total_cycles_per_unit")):
                    line[f"{prefix}_{csv_field}"] = serial[field]
                traffic = arm["logical_traffic"]
                for field in ("read_a_bytes_per_unit", "read_b_bytes_per_unit", "write_c_bytes_per_unit"):
                    line[f"{prefix}_{field}"] = traffic[field]
        writer.writerow(line)
    return output.getvalue()


def _metric_int(classified: dict[str, Any], mapping: str, metric: str) -> int:
    return classified["record"]["arms"][mapping][metric]


def _canonical_results(results: dict[int, dict[str, Any]], rows: list[dict[str, Any]]) -> dict[int, dict[str, Any]]:
    out = dict(results)
    for row in rows:
        if row["duplicate_of"] is not None and row["duplicate_of"] in results:
            source = results[row["duplicate_of"]]
            if source["runner_status"] == "complete":
                out[row["source_row_index"]] = source
    return out


def _v2_complete_record(row: dict[str, Any], results: dict[int, dict[str, Any]]) -> dict[str, Any] | None:
    item = results.get(row["source_row_index"])
    if item is None or item.get("runner_status") != "complete" or not isinstance(item.get("record"), dict):
        return None
    return item["record"]


def _v2_hypothesis(hypothesis_id: str, required: int, completed: int,
                   met: bool, contradicted: bool, evidence: list[dict[str, Any]]) -> dict[str, Any]:
    if met:
        status = "criterion_met"
    elif contradicted:
        status = "criterion_not_met"
    elif completed == required:
        status = "criterion_not_met"
    else:
        status = "inconclusive"
    return {
        "hypothesis_id": hypothesis_id,
        "status": status,
        "required_comparisons": required,
        "completed_comparisons": completed,
        "evidence": evidence,
    }


def _hypothesis_results_v2(manifest: dict[str, Any], rows: list[dict[str, Any]],
                           results: dict[int, dict[str, Any]]) -> list[dict[str, Any]]:
    by_base_sens = {(r["base_case_id"], r["sensitivity_case_id"]): r for r in rows}
    by_shape_level_sens = {(r["shape_id"], r["capacity_level_id"], r["sensitivity_case_id"]): r for r in rows}
    sensitivity = {c["sensitivity_case_id"]: c for c in manifest["sensitivity_cases"]}

    # H1: service-cycle difference accompanied by a mapping-derived traffic or batch difference.
    h1_evidence: list[dict[str, Any]] = []
    h1_completed = 0
    for row in rows:
        record = _v2_complete_record(row, results)
        if record is None:
            continue
        h1_completed += 1
        arms = record["arms"]
        traffic_fields = (
            "read_a_bytes_per_unit", "read_b_bytes_per_unit", "write_c_bytes_per_unit",
            "read_a_batches_per_unit", "read_b_batches_per_unit", "write_c_batches_per_unit",
        )
        traffic_diff = any(arms["rvv"]["logical_traffic"][f] != arms["matrix"]["logical_traffic"][f] for f in traffic_fields)
        rvv_service = sum(arms["rvv"]["serialized_backing_store"][f] for f in ("read_cycles_per_unit", "write_cycles_per_unit"))
        matrix_service = sum(arms["matrix"]["serialized_backing_store"][f] for f in ("read_cycles_per_unit", "write_cycles_per_unit"))
        if traffic_diff and rvv_service != matrix_service:
            h1_evidence.append({
                "candidate_pair_id": row["candidate_pair_id"], "study_row_id": row["study_row_id"],
                "request_sha256": row["ids"]["request_sha256"],
                "model_assumptions_sha256": row["ids"]["model_assumptions_sha256"],
                "rvv_read_plus_write_cycles": rvv_service,
                "matrix_read_plus_write_cycles": matrix_service,
                "result_path": row["result_path"],
            })
    h1 = _v2_hypothesis("H1", 24, h1_completed, bool(h1_evidence), False, h1_evidence)

    # H2: capacity alone changes between P4 and P8 for a fixed shape, sensitivity, and mapping.
    h2_completed = 0
    h2_evidence: list[dict[str, Any]] = []
    h2_counterexample = False
    for shape in manifest["shapes"]:
        for sid in sensitivity:
            low_row = by_shape_level_sens[(shape["shape_id"], "p4", sid)]
            high_row = by_shape_level_sens[(shape["shape_id"], "p8", sid)]
            low, high = _v2_complete_record(low_row, results), _v2_complete_record(high_row, results)
            if low is None or high is None:
                continue
            for mapping in ("rvv", "matrix"):
                h2_completed += 1
                lo = low["arms"][mapping]
                hi = high["arms"][mapping]
                passed = (hi["compute_cycles_per_unit"] < lo["compute_cycles_per_unit"] and
                          hi["serialized_backing_store"]["total_cycles_per_unit"] <
                          lo["serialized_backing_store"]["total_cycles_per_unit"])
                h2_counterexample = h2_counterexample or passed
                if passed:
                    h2_evidence.append({
                        "mapping": mapping, "p4_candidate_pair_id": low_row["candidate_pair_id"],
                        "p8_candidate_pair_id": high_row["candidate_pair_id"],
                        "p4_request_sha256": low_row["ids"]["request_sha256"],
                        "p8_request_sha256": high_row["ids"]["request_sha256"],
                        "p4_model_assumptions_sha256": low_row["ids"]["model_assumptions_sha256"],
                        "p8_model_assumptions_sha256": high_row["ids"]["model_assumptions_sha256"],
                        "result_paths": [low_row["result_path"], high_row["result_path"]],
                    })
    h2 = _v2_hypothesis("H2", 24, h2_completed, bool(h2_evidence), False, h2_evidence)

    # H3: higher bandwidth is a one-factor change; counterexamples invalidate the universal claim.
    h3_completed = 0
    h3_evidence: list[dict[str, Any]] = []
    h3_counterexample = False
    h3_strict_decrease = False
    for shape in manifest["shapes"]:
        for level in ("p4", "p8"):
            for setup in (0, 1):
                low_sid = f"bw_1e9_bytes_per_second_setup_{setup}"
                high_sid = f"bw_64e9_bytes_per_second_setup_{setup}"
                low_row = by_shape_level_sens[(shape["shape_id"], level, low_sid)]
                high_row = by_shape_level_sens[(shape["shape_id"], level, high_sid)]
                low, high = _v2_complete_record(low_row, results), _v2_complete_record(high_row, results)
                if low is None or high is None:
                    continue
                for mapping in ("rvv", "matrix"):
                    h3_completed += 1
                    lo = low["arms"][mapping]["serialized_backing_store"]
                    hi = high["arms"][mapping]["serialized_backing_store"]
                    fields = ("read_cycles_per_unit", "write_cycles_per_unit", "total_cycles_per_unit")
                    increased = any(hi[field] > lo[field] for field in fields)
                    decreased = any(hi[field] < lo[field] for field in fields[:2])
                    h3_counterexample = h3_counterexample or increased
                    h3_strict_decrease = h3_strict_decrease or decreased
                    if increased or decreased:
                        h3_evidence.append({
                            "mapping": mapping, "low_bandwidth_candidate_pair_id": low_row["candidate_pair_id"],
                            "high_bandwidth_candidate_pair_id": high_row["candidate_pair_id"],
                            "low_bandwidth_request_sha256": low_row["ids"]["request_sha256"],
                            "high_bandwidth_request_sha256": high_row["ids"]["request_sha256"],
                            "low_bandwidth_model_assumptions_sha256": low_row["ids"]["model_assumptions_sha256"],
                            "high_bandwidth_model_assumptions_sha256": high_row["ids"]["model_assumptions_sha256"],
                            "low_cycles": {field: lo[field] for field in fields},
                            "high_cycles": {field: hi[field] for field in fields},
                            "result_paths": [low_row["result_path"], high_row["result_path"]],
                        })
    h3_met = h3_completed == 24 and not h3_counterexample and h3_strict_decrease
    h3_not_met = h3_counterexample or (h3_completed == 24 and not h3_strict_decrease)
    h3 = _v2_hypothesis("H3", 24, h3_completed, h3_met, h3_not_met, h3_evidence)

    # H4: positive per-batch setup must not lower totals and must raise totals when returned batches exist.
    h4_completed = 0
    h4_evidence: list[dict[str, Any]] = []
    h4_counterexample = False
    for shape in manifest["shapes"]:
        for level in ("p4", "p8"):
            for bw in (1_000_000_000, 64_000_000_000):
                low_sid = f"bw_{bw // 1_000_000_000}e9_bytes_per_second_setup_0"
                high_sid = f"bw_{bw // 1_000_000_000}e9_bytes_per_second_setup_1"
                low_row = by_shape_level_sens[(shape["shape_id"], level, low_sid)]
                high_row = by_shape_level_sens[(shape["shape_id"], level, high_sid)]
                low, high = _v2_complete_record(low_row, results), _v2_complete_record(high_row, results)
                if low is None or high is None:
                    continue
                for mapping in ("rvv", "matrix"):
                    h4_completed += 1
                    lo_arm, hi_arm = low["arms"][mapping], high["arms"][mapping]
                    traffic = lo_arm["logical_traffic"]
                    nonzero_batches = any(traffic[f"{stem}_batches_per_unit"] > 0 for stem in ("read_a", "read_b", "write_c"))
                    lo_total = lo_arm["serialized_backing_store"]["total_cycles_per_unit"]
                    hi_total = hi_arm["serialized_backing_store"]["total_cycles_per_unit"]
                    violation = hi_total < lo_total or (nonzero_batches and hi_total <= lo_total)
                    h4_counterexample = h4_counterexample or violation
                    if nonzero_batches or violation:
                        h4_evidence.append({
                            "mapping": mapping, "zero_setup_candidate_pair_id": low_row["candidate_pair_id"],
                            "positive_setup_candidate_pair_id": high_row["candidate_pair_id"],
                            "zero_setup_request_sha256": low_row["ids"]["request_sha256"],
                            "positive_setup_request_sha256": high_row["ids"]["request_sha256"],
                            "zero_setup_model_assumptions_sha256": low_row["ids"]["model_assumptions_sha256"],
                            "positive_setup_model_assumptions_sha256": high_row["ids"]["model_assumptions_sha256"],
                            "nonzero_batches": nonzero_batches, "zero_setup_total_cycles": lo_total,
                            "positive_setup_total_cycles": hi_total,
                            "result_paths": [low_row["result_path"], high_row["result_path"]],
                        })
    h4_met = h4_completed == 24 and not h4_counterexample
    h4 = _v2_hypothesis("H4", 24, h4_completed, h4_met, h4_counterexample, h4_evidence)

    # H5: strict compute winner and strict serialized-total winner differ in a paired request.
    h5_completed = 0
    h5_evidence: list[dict[str, Any]] = []
    for row in rows:
        record = _v2_complete_record(row, results)
        if record is None:
            continue
        h5_completed += 1
        rvv, matrix = record["arms"]["rvv"], record["arms"]["matrix"]
        compute_winner = "rvv" if rvv["compute_cycles_per_unit"] < matrix["compute_cycles_per_unit"] else "matrix" if matrix["compute_cycles_per_unit"] < rvv["compute_cycles_per_unit"] else None
        rvv_total = rvv["serialized_backing_store"]["total_cycles_per_unit"]
        matrix_total = matrix["serialized_backing_store"]["total_cycles_per_unit"]
        serial_winner = "rvv" if rvv_total < matrix_total else "matrix" if matrix_total < rvv_total else None
        if compute_winner is not None and serial_winner is not None and compute_winner != serial_winner:
            h5_evidence.append({
                "candidate_pair_id": row["candidate_pair_id"], "study_row_id": row["study_row_id"],
                "request_sha256": row["ids"]["request_sha256"],
                "model_assumptions_sha256": row["ids"]["model_assumptions_sha256"],
                "compute_winner": compute_winner, "serialized_total_winner": serial_winner,
                "result_path": row["result_path"],
            })
    h5 = _v2_hypothesis("H5", 24, h5_completed, bool(h5_evidence), False, h5_evidence)
    return [h1, h2, h3, h4, h5]


def _hypothesis_results(manifest: dict[str, Any], rows: list[dict[str, Any]], results: dict[int, dict[str, Any]]) -> tuple[dict[str, Any], dict[str, Any]]:
    if manifest.get("schema_version") == 2:
        outcomes = _hypothesis_results_v2(manifest, rows, results)
        return outcomes[0], outcomes[1]
    by_group_level: dict[tuple[str, str], tuple[dict[str, Any], dict[str, Any]]] = {}
    row_lookup = {(r["comparison_group_id"], r["capacity_level_id"]): r for r in rows}
    normalized = _canonical_results(results, rows)
    capacity_supported = False
    capacity_pairs: list[str] = []
    all_capacity_complete = True
    for group in manifest["comparison_groups"]:
        group_rows = [row_lookup[(group["group_id"], level)] for level in ("p4", "p8")]
        low, high = group_rows
        lc = normalized.get(low["source_row_index"])
        hc = normalized.get(high["source_row_index"])
        if not lc or not hc or lc["runner_status"] != "complete" or hc["runner_status"] != "complete":
            all_capacity_complete = False
            continue
        by_group_level[(group["group_id"], "p4")] = (low, lc)
        by_group_level[(group["group_id"], "p8")] = (high, hc)
        for mapping in ("rvv", "matrix"):
            if _metric_int(hc, mapping, "compute_cycles_per_unit") < _metric_int(lc, mapping, "compute_cycles_per_unit"):
                capacity_supported = True
                capacity_pairs.extend([low["candidate_pair_id"], high["candidate_pair_id"]])
    if capacity_supported:
        capacity_status = "supported"
    elif all_capacity_complete:
        capacity_status = "contradicted"
    else:
        capacity_status = "inconclusive"
    traffic_supported = False
    traffic_pairs: list[str] = []
    all_traffic_complete = True
    for group in manifest["comparison_groups"]:
        for level in group["capacity_level_ids"]:
            row = row_lookup[(group["group_id"], level)]
            c = normalized.get(row["source_row_index"])
            if not c or c["runner_status"] != "complete":
                all_traffic_complete = False
                continue
            traffic_rvv = c["record"]["arms"]["rvv"]["logical_traffic"]
            traffic_matrix = c["record"]["arms"]["matrix"]["logical_traffic"]
            fields = ("read_a_bytes_per_unit", "read_b_bytes_per_unit", "write_c_bytes_per_unit")
            if any(traffic_rvv[field] != traffic_matrix[field] for field in fields):
                traffic_supported = True
                traffic_pairs.append(row["candidate_pair_id"])
    if traffic_supported:
        traffic_status = "supported"
    elif all_traffic_complete:
        traffic_status = "contradicted"
    else:
        traffic_status = "inconclusive"
    return (
        {"hypothesis_id": "capacity_sensitivity", "status": capacity_status, "candidate_pair_ids": sorted(set(capacity_pairs))},
        {"hypothesis_id": "mapping_traffic_difference", "status": traffic_status, "candidate_pair_ids": sorted(set(traffic_pairs))},
    )


def _v2_observed_findings(manifest: dict[str, Any], rows: list[dict[str, Any]],
                          results: dict[int, dict[str, Any]]) -> list[str]:
    """Describe comparisons from validated C++ fields; never estimate service cycles."""
    by_key = {(row["base_case_id"], row["sensitivity_case_id"]): row for row in rows}
    by_shape_capacity_sensitivity = {
        (row["shape_id"], row["capacity_level_id"], row["sensitivity_case_id"]): row
        for row in rows
    }
    complete_rows = [row for row in rows if _v2_complete_record(row, results) is not None]
    hypotheses = {item["hypothesis_id"]: item for item in _hypothesis_results_v2(manifest, rows, results)}
    lines = ["## What the model returned", ""]

    h1_example: tuple[dict[str, Any], dict[str, Any]] | None = None
    for row in complete_rows:
        record = _v2_complete_record(row, results)
        assert record is not None
        rvv = record["arms"]["rvv"]
        matrix = record["arms"]["matrix"]
        traffic_fields = (
            "read_a_bytes_per_unit", "read_b_bytes_per_unit", "write_c_bytes_per_unit",
            "read_a_batches_per_unit", "read_b_batches_per_unit", "write_c_batches_per_unit",
        )
        traffic_diff = any(rvv["logical_traffic"][field] != matrix["logical_traffic"][field] for field in traffic_fields)
        rvv_s = rvv["serialized_backing_store"]
        matrix_s = matrix["serialized_backing_store"]
        rvv_service = rvv_s["read_cycles_per_unit"] + rvv_s["write_cycles_per_unit"]
        matrix_service = matrix_s["read_cycles_per_unit"] + matrix_s["write_cycles_per_unit"]
        if traffic_diff and rvv_service != matrix_service:
            h1_example = (row, record)
            break
    h1 = hypotheses["H1"]
    if h1_example is not None:
        row, record = h1_example
        sensitivity = row["request"]["measurement"]["backing_store_sensitivity"]
        rvv, matrix = record["arms"]["rvv"], record["arms"]["matrix"]
        rvv_t, matrix_t = rvv["logical_traffic"], matrix["logical_traffic"]
        rvv_s, matrix_s = rvv["serialized_backing_store"], matrix["serialized_backing_store"]
        lines.append(
            f"- **H1 — mapping traffic and service:** {len(h1['evidence'])} qualifying rows among {h1['completed_comparisons']} complete paired rows showed different logical bytes/batch counts and different returned read-plus-write service cycles. For example, `{row['base_case_id']}` at `{row['sensitivity_case_id']}` used {sensitivity['bandwidth_bytes_per_second']} B/s and {sensitivity['batch_setup_cycles']} setup cycles/batch: RVV reported {rvv_t['total_bytes_per_unit']} logical bytes and {rvv_s['read_cycles_per_unit'] + rvv_s['write_cycles_per_unit']} read-plus-write cycles; matrix reported {matrix_t['total_bytes_per_unit']} logical bytes and {matrix_s['read_cycles_per_unit'] + matrix_s['write_cycles_per_unit']} cycles."
        )
    else:
        lines.append(f"- **H1 — mapping traffic and service:** {h1['status']} ({h1['completed_comparisons']}/24 paired rows complete); no qualifying example was available.")

    h2_count = 0
    h2_completed = 0
    p4_p8_examples: list[str] = []
    for shape in manifest["shapes"]:
        for case in manifest["sensitivity_cases"]:
            low_row = by_shape_capacity_sensitivity[(shape["shape_id"], "p4", case["sensitivity_case_id"])]
            high_row = by_shape_capacity_sensitivity[(shape["shape_id"], "p8", case["sensitivity_case_id"])]
            low = _v2_complete_record(low_row, results)
            high = _v2_complete_record(high_row, results)
            if low is None or high is None:
                continue
            for mapping in ("rvv", "matrix"):
                h2_completed += 1
                lo = low["arms"][mapping]
                hi = high["arms"][mapping]
                if (hi["compute_cycles_per_unit"] < lo["compute_cycles_per_unit"] and
                        hi["serialized_backing_store"]["total_cycles_per_unit"] < lo["serialized_backing_store"]["total_cycles_per_unit"]):
                    h2_count += 1
                    if len(p4_p8_examples) < 3:
                        p4_p8_examples.append(
                            f"{shape['shape_id']}/{case['sensitivity_case_id']}/{mapping}: compute {lo['compute_cycles_per_unit']}→{hi['compute_cycles_per_unit']}, serial total {lo['serialized_backing_store']['total_cycles_per_unit']}→{hi['serialized_backing_store']['total_cycles_per_unit']}"
                        )
    h2_examples = "; ".join(p4_p8_examples) if p4_p8_examples else "no qualifying P4/P8 example"
    lines.append(f"- **H2 — P4 to P8:** P8 lowered both compute-only and serialized total cycles in {h2_count} of {h2_completed}/24 matched shape/sensitivity/mapping comparisons. Examples: {h2_examples}.")

    h3_completed = 0
    h3_increase = 0
    h3_read_decreases = 0
    h3_write_decreases = 0
    h3_total_decreases = 0
    for shape in manifest["shapes"]:
        for level in ("p4", "p8"):
            for setup in (0, 1):
                low_row = by_key[(f"{shape['shape_id']}_{level}", f"bw_1e9_bytes_per_second_setup_{setup}")]
                high_row = by_key[(f"{shape['shape_id']}_{level}", f"bw_64e9_bytes_per_second_setup_{setup}")]
                low = _v2_complete_record(low_row, results)
                high = _v2_complete_record(high_row, results)
                if low is None or high is None:
                    continue
                for mapping in ("rvv", "matrix"):
                    h3_completed += 1
                    lo = low["arms"][mapping]["serialized_backing_store"]
                    hi = high["arms"][mapping]["serialized_backing_store"]
                    h3_increase += int(any(hi[field] > lo[field] for field in ("read_cycles_per_unit", "write_cycles_per_unit", "total_cycles_per_unit")))
                    h3_read_decreases += int(hi["read_cycles_per_unit"] < lo["read_cycles_per_unit"])
                    h3_write_decreases += int(hi["write_cycles_per_unit"] < lo["write_cycles_per_unit"])
                    h3_total_decreases += int(hi["total_cycles_per_unit"] < lo["total_cycles_per_unit"])
    lines.append(
        f"- **H3 — declared bandwidth:** Across {h3_completed}/24 matched mapping comparisons, increasing the declared input from 1 to 64 billion B/s produced {h3_increase} increases in read, write, or total cycles; read service decreased in {h3_read_decreases}, write service in {h3_write_decreases}, and total cycles in {h3_total_decreases}. This describes only the evaluator's returned serial estimate."
    )

    h4_completed = 0
    h4_increase = 0
    h4_deltas: list[int] = []
    for shape in manifest["shapes"]:
        for level in ("p4", "p8"):
            for bandwidth in (1_000_000_000, 64_000_000_000):
                low_row = by_key[(f"{shape['shape_id']}_{level}", f"bw_{bandwidth // 1_000_000_000}e9_bytes_per_second_setup_0")]
                high_row = by_key[(f"{shape['shape_id']}_{level}", f"bw_{bandwidth // 1_000_000_000}e9_bytes_per_second_setup_1")]
                low = _v2_complete_record(low_row, results)
                high = _v2_complete_record(high_row, results)
                if low is None or high is None:
                    continue
                for mapping in ("rvv", "matrix"):
                    h4_completed += 1
                    lo_arm, hi_arm = low["arms"][mapping], high["arms"][mapping]
                    delta = hi_arm["serialized_backing_store"]["total_cycles_per_unit"] - lo_arm["serialized_backing_store"]["total_cycles_per_unit"]
                    nonzero_batches = any(lo_arm["logical_traffic"][f"{stem}_batches_per_unit"] > 0 for stem in ("read_a", "read_b", "write_c"))
                    if nonzero_batches and delta > 0:
                        h4_increase += 1
                        h4_deltas.append(delta)
    delta_text = f"{min(h4_deltas)}–{max(h4_deltas)} cycles" if h4_deltas else "no positive delta observed"
    lines.append(f"- **H4 — setup cycles:** Positive setup increased the total in {h4_increase}/{h4_completed} matched comparisons with nonzero returned transfer batches; observed total-cycle increases ranged from {delta_text}.")

    h5 = hypotheses["H5"]
    reversals: list[str] = []
    for row in complete_rows:
        record = _v2_complete_record(row, results)
        assert record is not None
        rvv, matrix = record["arms"]["rvv"], record["arms"]["matrix"]
        compute_winner = "RVV" if rvv["compute_cycles_per_unit"] < matrix["compute_cycles_per_unit"] else "Matrix" if matrix["compute_cycles_per_unit"] < rvv["compute_cycles_per_unit"] else None
        rvv_total = rvv["serialized_backing_store"]["total_cycles_per_unit"]
        matrix_total = matrix["serialized_backing_store"]["total_cycles_per_unit"]
        serial_winner = "RVV" if rvv_total < matrix_total else "Matrix" if matrix_total < rvv_total else None
        if compute_winner and serial_winner and compute_winner != serial_winner:
            reversals.append(f"{row['base_case_id']}/{row['sensitivity_case_id']} ({compute_winner} compute vs {serial_winner} serial total: {rvv_total} vs {matrix_total} cycles)")
    lines.append(f"- **H5 — ranking changes:** {len(h5['evidence'])} of {h5['completed_comparisons']} complete paired requests had different strict compute and serial-total winners. " + ("Examples: " + "; ".join(reversals) + "." if reversals else "No strict-winner reversal occurred."))
    lines.append("")
    return lines


def _make_report_v2(manifest: dict[str, Any], rows: list[dict[str, Any]],
                    results: dict[int, dict[str, Any]], study_status: str) -> str:
    hypotheses = _hypothesis_results_v2(manifest, rows, results)
    outcome_by_id = {item["hypothesis_id"]: item for item in hypotheses}
    lines = [
        "# Paired GEMM serialized backing-store sensitivity study", "",
        f"Study label: `{manifest['study_label']}`. Study status: **{study_status}**.", "",
        "This is a deterministic sensitivity calculation using the existing C++ compute mappings and its declared serial-service equation. Every cycle count below is an analytical model output under the stated assumptions; it is not measured latency, physical memory performance, or hardware speedup.", "",
        "## Fixed study assumptions", "",
        "- Shapes, mapping parameters, P4/P8 abstract compute capacities, signed-i16/signed-i32 shape-only workload semantics, and 1 GHz clock are inherited unchanged from the accepted M6.3c manifest.",
            "- P4/P8 are abstract compute-capacity settings, not equal-area, equal-power, or equal-cost hardware alternatives.",
        "- Each sensitivity case supplies one declared decimal bandwidth and one per-batch setup-cycle value to both arms of a paired C++ request.",
        "- Sensitivity values are illustrative analytical inputs, not device-characterized values or statistical uncertainty ranges.",
        "- C++ owns all computed cycles and logical byte/batch counts. Python validates returned fields and forms comparisons; it does not reproduce the read/write service equation.",
        "- The accepted M6.3c reference is retained read-only. Sensitivity-bearing request, pair, and model-assumption identities are intentionally distinct.",
        "",
        "| Sensitivity ID | Declared bandwidth (B/s) | Setup (cycles/batch) |",
        "|---|---:|---:|",
    ]
    for case in manifest["sensitivity_cases"]:
        lines.append(f"| `{case['sensitivity_case_id']}` | {case['bandwidth_bytes_per_second']} | {case['batch_setup_cycles']} |")
    reference = manifest["baseline_reference"]
    lines += [
        "", "Accepted M6.3c reference identity:",
        f"- Study: `{reference['study_label']}`; manifest ID: `{reference['manifest_id']}`.",
        f"- Canonical artifact manifest SHA-256: `{reference['canonical_artifact_manifest_sha256']}`.",
        f"- Source revision: `{reference['source_revision']}`; study run ID: `{reference['study_run_id']}`.",
        f"- Preserved bundle tar SHA-256: `{reference['bundle_tar_sha256']}`.", "",
              "## Preregistered outcomes", "",
              "H1–H4 are deterministic checks of fields returned by the model. H5 is a preregistered scenario check. `criterion_met` means only that the declared condition occurred in these model outputs; it is not empirical validation.", "",
              "| ID | Preregistered statement | Outcome | Completed / required |",
              "|---|---|---|---:|"]
    for hyp in manifest["hypotheses"]:
        outcome = outcome_by_id[hyp["hypothesis_id"]]
        lines.append(f"| {hyp['hypothesis_id']} | {hyp['statement']} | **{outcome['status']}** | {outcome['completed_comparisons']} / {outcome['required_comparisons']} |")
    lines += ["", "### Evidence references", ""]
    for hid in ("H1", "H2", "H3", "H4", "H5"):
        outcome = outcome_by_id[hid]
        evidence = outcome["evidence"]
        if not evidence:
            lines.append(f"- **{hid}:** no qualifying complete comparison. Missing, failed, partial, timed-out, invalid, or unrun comparisons remain inconclusive where required.")
            continue
        pointer_index = int(hid[1:]) - 1
        lines.append(f"- **{hid}:** {len(evidence)} qualifying or diagnostic comparison(s). Candidate/request identities and result paths are listed at `summary.json#/hypotheses/{pointer_index}/evidence`; each result path points to the corresponding C++ result record without identity normalization.")
    lines += [""] + _v2_observed_findings(manifest, rows, results)
    lines += [
        "", "## Complete outcome ledger", "",
        "Every planned candidate is represented below, including candidates that did not run or did not produce a complete pair. Only rows with both C++ arms complete contribute to hypothesis decisions and plots.", "",
        "| Row | Base | Sensitivity | P | Status | Candidate pair ID | Request SHA-256 | Assumptions SHA-256 | RVV compute / serial total (cycles/unit) | Matrix compute / serial total (cycles/unit) | C++ result pointer |",
        "|---:|---|---|---|---|---|---|---|---:|---:|---|",
    ]
    for row in rows:
        classified = results.get(row["source_row_index"], {})
        status = classified.get("runner_status")
        if status is None:
            status = "not_started_due_to_preflight_failure" if study_status == "pilot_failed" else "not_started_due_to_limit"
        record = classified.get("record") if status == "complete" else None
        if isinstance(record, dict):
            rvv = record["arms"]["rvv"]
            matrix = record["arms"]["matrix"]
            rvv_text = f"{rvv['compute_cycles_per_unit']} / {rvv['serialized_backing_store']['total_cycles_per_unit']}"
            matrix_text = f"{matrix['compute_cycles_per_unit']} / {matrix['serialized_backing_store']['total_cycles_per_unit']}"
            result_pointer = f"`{row['result_path']}#/arms/rvv/serialized_backing_store`<br>`{row['result_path']}#/arms/matrix/serialized_backing_store`"
        else:
            rvv_text = matrix_text = "n/a"
            artifact_paths = classified.get("artifact_paths", [])
            result_pointer = f"`{artifact_paths[-1]}`" if artifact_paths else "not produced"
        sensitivity = row["request"]["measurement"]["backing_store_sensitivity"]
        sensitivity_label = row["sensitivity_case_id"]
        p = row["request"]["mappings"]["matrix"]["mac_slots_per_cycle"]
        lines.append(
            f"| {row['source_row_index']} | `{row['base_case_id']}` | `{sensitivity_label}` ({sensitivity['bandwidth_bytes_per_second']} B/s, {sensitivity['batch_setup_cycles']} cycles/batch) | {p} | `{status}` | `{row['candidate_pair_id']}` | `{row['ids']['request_sha256']}` | `{row['ids']['model_assumptions_sha256']}` | {rvv_text} | {matrix_text} | {result_pointer} |"
        )
    lines += [
        "", "## Interpretation boundaries", "",
        "The serialized total is the C++ model's read-service + compute + write-service sequence. A/B reads use the evaluator's aggregated ceiling-rounded read term; C output writes use a separate term; batch setup is attributed using the returned read and write batch counts. The service sequence has no compute/memory overlap.",
        "",
        "This study does not model compute/memory overlap, memory arbitration or queues, cache hits or memory residency, DRAM commands, physical transfer latency, memory energy or power, physical bottlenecks, or AME architectural semantics. Logical A/B/C payload bytes are not physical transfers. Positive setup can remain as a bandwidth-independent term; finite bandwidth also leaves ceiling-rounded cycles for nonzero transfer groups. The serial total is not expected to converge to compute-only cycles.",
        "",
        "No conclusion here establishes physical silicon performance, hardware speedup, power/energy efficiency, or a physical memory bottleneck. The results only show what the existing serialized sensitivity model returns under these declared assumptions.",
        "",
        "Reproduce with the v2 manifest using the documented `plan`, `preflight`, `execute`, and `verify` commands. The canonical artifact manifest and bundle index bind file hashes; host evaluator diagnostics link launches to study rows. The complete evidence is preserved in the manifest, plan, ledger, C++ result records, summary, plots, and provenance files.",
    ]
    text = "\n".join(lines) + "\n"
    if len(text.encode("utf-8")) > V2_FILE_CAPS["report.md"]:
        _fail("output_limit", "M6.3d report exceeds its 32 KiB cap", exit_code=5)
    return text


def _make_report(manifest: dict[str, Any], rows: list[dict[str, Any]], results: dict[int, dict[str, Any]], study_status: str, *, failure_index: int | None = None) -> str:
    if manifest.get("schema_version") == 2:
        return _make_report_v2(manifest, rows, results, study_status)
    h1, h2 = _hypothesis_results(manifest, rows, results)
    shape_lookup = {shape["shape_id"]: shape for shape in manifest["shapes"]}
    evidence: list[str] = []
    normalized = _canonical_results(results, rows)
    row_lookup = {(r["comparison_group_id"], r["capacity_level_id"]): r for r in rows}
    for group in manifest["comparison_groups"]:
        low_row = row_lookup[(group["group_id"], "p4")]
        high_row = row_lookup[(group["group_id"], "p8")]
        low = normalized.get(low_row["source_row_index"])
        high = normalized.get(high_row["source_row_index"])
        if low and high and low.get("runner_status") == "complete" and high.get("runner_status") == "complete":
            for mapping in ("rvv", "matrix"):
                low_cycles = _metric_int(low, mapping, "compute_cycles_per_unit")
                high_cycles = _metric_int(high, mapping, "compute_cycles_per_unit")
                evidence.append(f"- Capacity: `{low_row['candidate_pair_id']}` `/arms/{mapping}/compute_cycles_per_unit` ({low_cycles}) to `{high_row['candidate_pair_id']}` `/arms/{mapping}/compute_cycles_per_unit` ({high_cycles}).")
    for row in rows:
        if row["duplicate_of"] is not None:
            continue
        candidate = normalized.get(row["source_row_index"])
        if not candidate or candidate.get("runner_status") != "complete":
            continue
        traffic = candidate["record"]["arms"]
        for field in ("read_a_bytes_per_unit", "read_b_bytes_per_unit", "write_c_bytes_per_unit"):
            rvv_bytes = traffic["rvv"]["logical_traffic"][field]
            matrix_bytes = traffic["matrix"]["logical_traffic"][field]
            evidence.append(f"- Traffic: `{row['candidate_pair_id']}` `/arms/rvv/logical_traffic/{field}` ({rvv_bytes}) and `/arms/matrix/logical_traffic/{field}` ({matrix_bytes}).")
    lines = [
        "# Paired GEMM mapping study",
        "",
        f"Study label: `{manifest['study_label']}`. Status: `{study_status}`.",
        "",
        "This bundle reports C++ analytical mapping outputs for the declared shape-only GEMM work units. It does not report measured hardware performance.",
        "",
        "## Hypotheses",
        "",
        "### Capacity sensitivity",
        "",
        f"Preregistered statement: `{HYPOTHESES[0]['statement']}`",
        "Decision rule: supported if any complete P4/P8 group has lower C++ compute cycles at P8 for either mapping; contradicted only if every declared group completes and no mapping has a reduction; otherwise inconclusive.",
        f"Outcome: **{h1['status']}**. Candidate pair IDs: {', '.join(h1['candidate_pair_ids']) if h1['candidate_pair_ids'] else 'none'}.",
        "",
        "### Mapping traffic difference",
        "",
        f"Preregistered statement: `{HYPOTHESES[1]['statement']}`",
        "Decision rule: supported if any complete candidate has a byte-field difference; contradicted only if every declared candidate completes and all three byte fields match; otherwise inconclusive.",
        f"Outcome: **{h2['status']}**. Candidate pair IDs: {', '.join(h2['candidate_pair_ids']) if h2['candidate_pair_ids'] else 'none'}.",
        "",
        "The decision rules use only complete paired C++ results. A partial arm, invalid request, failure, timeout, identity mismatch, or unstarted row cannot count as a complete comparison.",
        "",
        "## Candidate ledger summary",
        "",
        "| Row | Candidate pair | Group | Shape M×N×K | Matrix tile M×N×K | Matrix local capacity bytes | P (RVV lanes / matrix slots per cycle) | RVV VLEN/SEW/LMUL | Status | RVV cycles | Matrix cycles | RVV A/B/C bytes | Matrix A/B/C bytes |",
        "|---:|---|---|---:|---:|---:|---:|---|---|---:|---:|---:|---:|",
    ]
    normalized = _canonical_results(results, rows)
    for row in rows:
        c = results.get(row["source_row_index"])
        if row["duplicate_of"] is not None:
            status = "duplicate"
        elif c:
            status = c["runner_status"]
        elif study_status == "provenance_failed":
            status = "not_started_provenance_failure"
        elif study_status == "pilot_failed":
            status = "not_started_due_to_preflight_failure"
        else:
            status = "not_started_due_to_limit"
        metric = normalized.get(row["source_row_index"])
        if metric and metric["runner_status"] == "complete":
            rvv_arm = metric["record"]["arms"]["rvv"]
            matrix_arm = metric["record"]["arms"]["matrix"]
            rvv = rvv_arm["compute_cycles_per_unit"]
            matrix = matrix_arm["compute_cycles_per_unit"]
            rvv_traffic = rvv_arm["logical_traffic"]
            matrix_traffic = matrix_arm["logical_traffic"]
            rvv_abc = "/".join(str(rvv_traffic[f"{key}_bytes_per_unit"]) for key in ("read_a", "read_b", "write_c"))
            matrix_abc = "/".join(str(matrix_traffic[f"{key}_bytes_per_unit"]) for key in ("read_a", "read_b", "write_c"))
        else:
            rvv = matrix = "n/a"
            rvv_abc = matrix_abc = "n/a"
        shape = shape_lookup[row["shape_id"]]
        dims = f"{shape['m']}×{shape['n']}×{shape['k']}"
        tile = f"{shape['tile_m']}×{shape['tile_n']}×{shape['tile_k']}"
        mapping = row["request"]["mappings"]
        rvv_params = mapping["rvv"]
        rvv_vlen = f"{rvv_params['vlen_bits']}/{rvv_params['sew_bits']}/{rvv_params['lmul']}"
        matrix_slots = mapping["matrix"]["mac_slots_per_cycle"]
        matrix_local_bytes = mapping["matrix"]["local_capacity_bytes"]
        capacity = f"{row['capacity_level_id'].upper()} ({rvv_params['lanes']} / {matrix_slots})"
        pair_id = row["candidate_pair_id"] or "n/a"
        lines.append(f"| {row['source_row_index']} | `{pair_id}` | `{row['comparison_group_id']}` | {dims} | {tile} | {matrix_local_bytes} | {capacity} | {rvv_vlen} | `{status}` | {rvv} | {matrix} | {rvv_abc} | {matrix_abc} |")
    failures: list[tuple[dict[str, Any], dict[str, Any] | None]] = []
    for row in rows:
        if row["duplicate_of"] is not None:
            continue
        classified = results.get(row["source_row_index"])
        if classified is None or classified.get("runner_status") != "complete":
            failures.append((row, classified))
    if failures:
        lines += ["", "## Failed, partial, and unstarted rows", "", "| Row | Status | Stable error codes |", "|---:|---|---|"]
        for row, classified in failures:
            missing_status = "not_started_provenance_failure" if study_status == "provenance_failed" else "not_started_due_to_preflight_failure" if study_status == "pilot_failed" else "not_started_due_to_limit"
            status = classified.get("runner_status", missing_status) if classified else missing_status
            if classified is None:
                codes = "provenance_failed" if study_status == "provenance_failed" else "output_limit" if study_status == "output_limit" else "active_time_limit" if study_status == "limit_reached" else "pilot_failed" if study_status == "pilot_failed" and failure_index is not None and row["source_row_index"] > failure_index else "pilot_not_run" if study_status == "pilot_failed" else missing_status
            else:
                codes = ", ".join(classified.get("error_codes", []))
            lines.append(f"| {row['source_row_index']} | `{status}` | `{codes or 'none'}` |")
    lines += [
        "",
        "## Comparison boundaries",
        "",
        "- P4 and P8 are abstract MAC-slot rates. The comparison does not match area, power, or silicon cost.",
        "- Cycles are analytical model cycles at the declared 1 GHz clock. Any quotient of cycle counts is a modeled ratio, not measured speedup.",
        "- A/B/C values are mapping-derived logical payload bytes; they are not physical transactions, SRAM actions, DRAM commands, or energy.",
        "- The three shapes use different tile geometries, so cross-shape differences do not isolate shape alone.",
        "- The work unit is shape-only: the evaluator does not load operand values or calculate matrix outputs.",
        "- Power, energy, area, AME semantics, CPU dispatch, overlap, caches, shared-memory arbitration, and hardware variability are not modeled.",
        "- This is not evidence about any vendor's implementation or physical silicon.",
        "",
        "## Result evidence",
        "",
    ]
    if h1["status"] == "supported":
        lines.append("Capacity sensitivity evidence is the C++ `compute_cycles_per_unit` field at the P4/P8 rows named above.")
    if h2["status"] == "supported":
        lines.append("Logical payload evidence is the C++ `logical_traffic` A-read, B-read, or C-write fields for the named result records.")
    lines.append("Exact result JSON pointers supporting the findings:")
    lines.extend(evidence or ["- No complete result supports either hypothesis yet."])
    lines.append("Inspect `runner-ledger.jsonl`, `summary.json`, and the per-candidate result files for the full status and identity chain.")
    return "\n".join(lines) + "\n"


def _svg_text(value: str) -> str:
    return xml_escape.escape(value, {"\"": "&quot;"})


def _svg_bar_chart(title: str, ylabel: str, rows: list[tuple[str, int, int]], caption: str, *, colors: tuple[str, str] = ("#e76f51", "#69aab0")) -> str:
    width = 1100
    left, right, top, bottom = 250, 150, 60, 95
    group_h = max(34, min(54, (520 - top - bottom) // max(1, len(rows))))
    height = max(520, top + bottom + len(rows) * group_h)
    plot_w = width - left - right
    plot_h = height - top - bottom
    maximum = max((max(a, b) for _, a, b in rows), default=1) or 1
    bar_h = 12
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">', '<rect width="100%" height="100%" fill="#f7f4ed"/>', f'<text x="{left}" y="30" font-family="Arial,sans-serif" font-size="21" font-weight="700" fill="#202522">{_svg_text(title)}</text>', f'<text x="22" y="{top + plot_h//2}" transform="rotate(-90 22 {top + plot_h//2})" font-family="Arial,sans-serif" font-size="14" fill="#38413e">{_svg_text(ylabel)}</text>']
    for index in range(6):
        x = left + round(plot_w * index / 5)
        value = round(maximum * index / 5)
        parts.append(f'<line x1="{x}" y1="{top}" x2="{x}" y2="{top+plot_h}" stroke="#d1d4ce" stroke-width="1"/>')
        parts.append(f'<text x="{x}" y="{top+plot_h+20}" text-anchor="middle" font-family="Arial,sans-serif" font-size="11" fill="#38413e">{value}</text>')
    if rows:
        for index, (label, a, b) in enumerate(rows):
            y = top + index * group_h + 4
            parts.append(f'<text x="{left-12}" y="{y+19}" text-anchor="end" font-family="Arial,sans-serif" font-size="12" fill="#202522">{_svg_text(label)}</text>')
            for offset, value, color, name in ((0, a, colors[0], "RVV"), (bar_h+4, b, colors[1], "Generic matrix")):
                bar_w = max(1, round(plot_w * value / maximum))
                parts.append(f'<rect x="{left}" y="{y+offset}" width="{bar_w}" height="{bar_h}" fill="{color}"/>')
                label_x = left + bar_w + 5
                anchor = "start"
                if label_x > width - 8:
                    label_x = width - 8
                    anchor = "end"
                parts.append(f'<text x="{label_x}" y="{y+offset+10}" text-anchor="{anchor}" font-family="Arial,sans-serif" font-size="10" fill="#202522">{value}</text>')
    else:
        parts.append(f'<text x="{left+plot_w//2}" y="{top+plot_h//2}" text-anchor="middle" font-family="Arial,sans-serif" font-size="16" fill="#38413e">No complete paired results</text>')
    legend_y = height - 52
    parts.extend([
        f'<rect x="{left}" y="{legend_y}" width="14" height="14" fill="{colors[0]}"/><text x="{left+21}" y="{legend_y+12}" font-family="Arial,sans-serif" font-size="12" fill="#202522">RVV</text>',
        f'<rect x="{left+90}" y="{legend_y}" width="14" height="14" fill="{colors[1]}"/><text x="{left+111}" y="{legend_y+12}" font-family="Arial,sans-serif" font-size="12" fill="#202522">Generic tiled matrix</text>',
        f'<text x="{left}" y="{height-15}" font-family="Arial,sans-serif" font-size="11" fill="#38413e">{_svg_text(caption)}</text>',
        "</svg>",
    ])
    return "\n".join(parts) + "\n"


def _make_plots(rows: list[dict[str, Any]], results: dict[int, dict[str, Any]]) -> tuple[bytes, bytes]:
    normalized = _canonical_results(results, rows)
    compute_rows: list[tuple[str, int, int]] = []
    logical_rows: list[tuple[str, int, int]] = []
    for row in rows:
        if row["duplicate_of"] is not None:
            continue
        c = normalized.get(row["source_row_index"])
        if not c or c["runner_status"] != "complete":
            continue
        rec = c["record"]
        label = f"{row['shape_id']} / {row['capacity_level_id'].upper()}"
        compute_rows.append((label, rec["arms"]["rvv"]["compute_cycles_per_unit"], rec["arms"]["matrix"]["compute_cycles_per_unit"]))
        traffic_rvv = rec["arms"]["rvv"]["logical_traffic"]
        traffic_matrix = rec["arms"]["matrix"]["logical_traffic"]
        for category, key in (("A read", "read_a_bytes_per_unit"), ("B read", "read_b_bytes_per_unit"), ("C write", "write_c_bytes_per_unit")):
            logical_rows.append((f"{label} · {category}", traffic_rvv[key], traffic_matrix[key]))
    plot1 = _svg_bar_chart(
        "Analytical compute cycles per work unit", "Modeled compute cycles",
        compute_rows, "Analytical mapping cycles; not measured hardware latency.",
    ).encode("utf-8")
    plot2 = _svg_bar_chart(
        "Mapping-derived logical payload bytes", "Logical payload bytes",
        logical_rows, "Logical payload only; not physical transfers, SRAM actions, DRAM commands, or energy.",
        colors=("#d89b35", "#6b87ba"),
    ).encode("utf-8")
    return plot1, plot2


def _make_v2_plots(rows: list[dict[str, Any]], results: dict[int, dict[str, Any]]) -> dict[str, bytes]:
    complete = {
        row["source_row_index"]: results[row["source_row_index"]]["record"]
        for row in rows
        if row["duplicate_of"] is None
        and row["source_row_index"] in results
        and results[row["source_row_index"]].get("runner_status") == "complete"
    }
    # Compute and logical traffic are sensitivity-invariant; retain one complete
    # C++ record per base case so the legacy plots remain a six-case view.
    base_rows: list[dict[str, Any]] = []
    seen_base: set[str] = set()
    for row in rows:
        if row["base_case_id"] in seen_base or row["source_row_index"] not in complete:
            continue
        base_rows.append(row)
        seen_base.add(row["base_case_id"])
    compute_plot, traffic_plot = _make_plots(base_rows, results)

    base_order = list(dict.fromkeys(row["base_case_id"] for row in rows))
    lookup = {(row["base_case_id"], row["sensitivity_case_id"]): row for row in rows}
    sensitivity_order = [case["sensitivity_case_id"] for case in V2_SENSITIVITY_CASES]
    sensitivity_values = {case["sensitivity_case_id"]: case for case in V2_SENSITIVITY_CASES}

    # Discrete endpoint chart: markers only (no line between bandwidth values).
    width, height = 1240, 760
    panel_w, panel_h = 390, 310
    colors = {("rvv", 0): "#d65f3c", ("matrix", 0): "#287f86", ("rvv", 1): "#b83f2d", ("matrix", 1): "#165967"}
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f7f4ed"/>',
        '<text x="28" y="34" font-family="Arial,sans-serif" font-size="22" font-weight="700" fill="#202522">Serialized modeled total cycles by declared bandwidth</text>',
        '<text x="28" y="56" font-family="Arial,sans-serif" font-size="12" fill="#38413e">Discrete sensitivity endpoints; markers are not a continuous curve. Horizontal dashed lines are compute-only references.</text>',
    ]
    for panel_index, base_id in enumerate(base_order):
        col, row_idx = panel_index % 3, panel_index // 3
        ox, oy = 18 + col * panel_w, 72 + row_idx * panel_h
        shape_id, level_id = base_id.rsplit("_", 1)
        panel_rows = [lookup[(base_id, sid)] for sid in sensitivity_order]
        panel_records = [complete.get(candidate["source_row_index"]) for candidate in panel_rows]
        max_cycles = max(
            (max(record["arms"][mapping]["serialized_backing_store"]["total_cycles_per_unit"], record["arms"][mapping]["compute_cycles_per_unit"])
             for record in panel_records if record is not None for mapping in ("rvv", "matrix")),
            default=1,
        ) or 1
        left, top, plot_w, plot_h = ox + 50, oy + 36, 300, 210
        lines.append(f'<rect x="{ox}" y="{oy}" width="374" height="280" fill="none" stroke="#7c837d"/>')
        lines.append(f'<text x="{ox+12}" y="{oy+22}" font-family="Arial,sans-serif" font-size="15" font-weight="700" fill="#202522">{_svg_text(shape_id)} / {level_id.upper()}</text>')
        for tick in range(4):
            y = top + round(plot_h * tick / 3)
            value = round(max_cycles * (3 - tick) / 3)
            lines.append(f'<line x1="{left}" y1="{y}" x2="{left+plot_w}" y2="{y}" stroke="#d1d4ce"/>')
            lines.append(f'<text x="{left-7}" y="{y+4}" text-anchor="end" font-family="Arial,sans-serif" font-size="9" fill="#38413e">{value}</text>')
        x_for_bandwidth = {1_000_000_000: left + 75, 64_000_000_000: left + 225}
        for mapping in ("rvv", "matrix"):
            values = [record["arms"][mapping]["compute_cycles_per_unit"] for record in panel_records if record is not None]
            if values:
                comp = values[0]
                y = top + plot_h - round(plot_h * comp / max_cycles)
                lines.append(f'<line x1="{left}" y1="{y}" x2="{left+plot_w}" y2="{y}" stroke="{colors[(mapping, 0)]}" stroke-dasharray="5,4" opacity="0.65"/>')
        for candidate in panel_rows:
            record = complete.get(candidate["source_row_index"])
            if record is None:
                continue
            sensitivity = sensitivity_values[candidate["sensitivity_case_id"]]
            bw = sensitivity["bandwidth_bytes_per_second"]
            setup = sensitivity["batch_setup_cycles"]
            x = x_for_bandwidth[bw]
            for mapping in ("rvv", "matrix"):
                cycles = record["arms"][mapping]["serialized_backing_store"]["total_cycles_per_unit"]
                y = top + plot_h - round(plot_h * cycles / max_cycles)
                color = colors[(mapping, setup)]
                lines.append(f'<circle cx="{x}" cy="{y}" r="5" fill="{color}" stroke="#202522" stroke-width="0.6"><title>{_svg_text(candidate["sensitivity_case_id"])} {mapping}: {cycles} modeled serial cycles</title></circle>')
        lines.append(f'<text x="{x_for_bandwidth[1_000_000_000]}" y="{top+plot_h+18}" text-anchor="middle" font-family="Arial,sans-serif" font-size="10" fill="#202522">1 GB/s</text>')
        lines.append(f'<text x="{x_for_bandwidth[64_000_000_000]}" y="{top+plot_h+18}" text-anchor="middle" font-family="Arial,sans-serif" font-size="10" fill="#202522">64 GB/s</text>')
    legend = [
        (("rvv", 0), "RVV setup 0 cycles/batch"), (("matrix", 0), "Matrix setup 0 cycles/batch"),
        (("rvv", 1), "RVV setup 1 cycle/batch"), (("matrix", 1), "Matrix setup 1 cycle/batch"),
    ]
    for index, (key, label) in enumerate(legend):
        x = 32 + index * 190
        lines.append(f'<circle cx="{x}" cy="720" r="5" fill="{colors[key]}"/><text x="{x+12}" y="724" font-family="Arial,sans-serif" font-size="11" fill="#202522">{label}</text>')
    lines.append('<text x="820" y="724" font-family="Arial,sans-serif" font-size="11" fill="#202522">Dashed lines: compute-only reference for each mapping. All values are modeled serial cycles, not latency.</text>')
    lines.append("</svg>")
    total_plot = ("\n".join(lines) + "\n").encode("utf-8")

    # Six compact panels; each has four sensitivity groups and two stacked bars.
    width, height, panel_w, panel_h = 1240, 820, 400, 360
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f7f4ed"/>',
        '<text x="26" y="32" font-family="Arial,sans-serif" font-size="22" font-weight="700" fill="#202522">Serialized cycle component breakdown</text>',
        '<text x="26" y="52" font-family="Arial,sans-serif" font-size="12" fill="#38413e">Modeled read service + compute + write service; bandwidth uses decimal GB/s and setup is cycles/batch.</text>',
    ]
    component_colors = {"read": "#d89b35", "compute": "#287f86", "write": "#6b87ba"}
    for panel_index, base_id in enumerate(base_order):
        col, row_idx = panel_index % 3, panel_index // 3
        ox, oy = 10 + col * panel_w, 68 + row_idx * panel_h
        shape_id, level_id = base_id.rsplit("_", 1)
        panel_rows = [lookup[(base_id, sid)] for sid in sensitivity_order]
        panel_records = [complete.get(candidate["source_row_index"]) for candidate in panel_rows]
        maximum = max((record["arms"][mapping]["serialized_backing_store"]["total_cycles_per_unit"] for record in panel_records if record for mapping in ("rvv", "matrix")), default=1) or 1
        left, top, plot_w, plot_h = ox + 36, oy + 34, 346, 250
        parts.append(f'<rect x="{ox}" y="{oy}" width="390" height="330" fill="none" stroke="#7c837d"/>')
        parts.append(f'<text x="{ox+10}" y="{oy+21}" font-family="Arial,sans-serif" font-size="14" font-weight="700" fill="#202522">{_svg_text(shape_id)} / {level_id.upper()}</text>')
        for tick in range(4):
            y = top + round(plot_h * tick / 3)
            val = round(maximum * (3 - tick) / 3)
            parts.append(f'<line x1="{left}" y1="{y}" x2="{left+plot_w}" y2="{y}" stroke="#d1d4ce"/>')
            parts.append(f'<text x="{left-5}" y="{y+3}" text-anchor="end" font-family="Arial,sans-serif" font-size="8" fill="#38413e">{val}</text>')
        for group_idx, candidate in enumerate(panel_rows):
            record = complete.get(candidate["source_row_index"])
            gx = left + 13 + group_idx * 82
            if record is not None:
                for offset, mapping in ((0, "rvv"), (31, "matrix")):
                    serial = record["arms"][mapping]["serialized_backing_store"]
                    x = gx + offset
                    y_cursor = top + plot_h
                    for comp, field in (("read", "read_cycles_per_unit"), ("compute", "compute_cycles_per_unit"), ("write", "write_cycles_per_unit")):
                        value = serial[field]
                        bar_height = round(plot_h * value / maximum)
                        y_cursor -= bar_height
                        parts.append(f'<rect x="{x}" y="{y_cursor}" width="23" height="{max(1, bar_height)}" fill="{component_colors[comp]}"><title>{mapping} {comp}: {value} cycles</title></rect>')
            sensitivity = sensitivity_values[candidate["sensitivity_case_id"]]
            bw_label = "1 GB/s" if sensitivity["bandwidth_bytes_per_second"] == 1_000_000_000 else "64 GB/s"
            setup_label = sensitivity["batch_setup_cycles"]
            parts.append(f'<text x="{gx+27}" y="{top+plot_h+12}" text-anchor="middle" font-family="Arial,sans-serif" font-size="7" fill="#202522">{bw_label}</text>')
            parts.append(f'<text x="{gx+27}" y="{top+plot_h+22}" text-anchor="middle" font-family="Arial,sans-serif" font-size="7" fill="#202522">{setup_label} cycles/batch</text>')
        parts.append(f'<text x="{left+13}" y="{top+plot_h+32}" font-family="Arial,sans-serif" font-size="8" fill="#38413e">RVV / Matrix bars per condition</text>')
    for index, (name, color) in enumerate(component_colors.items()):
        x = 34 + index * 150
        legend_label = {"read": "Read service cycles", "compute": "Compute cycles", "write": "Write service cycles"}[name]
        parts.append(f'<rect x="{x}" y="800" width="12" height="12" fill="{color}"/><text x="{x+18}" y="810" font-family="Arial,sans-serif" font-size="11" fill="#202522">{legend_label}</text>')
    parts.append('<text x="520" y="810" font-family="Arial,sans-serif" font-size="11" fill="#202522">Component values are modeled serial cycles under assumptions; not latency or physical transfer behavior.</text>')
    parts.append("</svg>")
    breakdown_plot = ("\n".join(parts) + "\n").encode("utf-8")
    return {
        "plots/compute-cycles.svg": compute_plot,
        "plots/logical-payload-bytes.svg": traffic_plot,
        "plots/serialized-total-cycles.svg": total_plot,
        "plots/serialized-cycle-breakdown.svg": breakdown_plot,
    }


def _write_terminal_preflight_bundle(store: ArtifactStore, manifest: dict[str, Any],
                                     rows: list[dict[str, Any]],
                                     results: dict[int, dict[str, Any]], status: str,
                                     study_run_id: str, diagnostics: list[dict[str, Any]],
                                     failure_index: int | None = None) -> None:
    caps = _caps_for_manifest(manifest)
    _write_preflight_ledger(store, rows, results, failure_index, status)
    summary = _build_summary(manifest, rows, results, status, study_run_id, failure_index=failure_index)
    store.write_json("summary.json", summary, cap=caps["summary.json"])
    store.write("summary.csv", _summary_csv(rows, results, summary["rows"]).encode("ascii"), cap=caps["summary.csv"])
    store.write("report.md", _make_report(manifest, rows, results, status, failure_index=failure_index).encode("utf-8"),
                cap=caps["report.md"])
    plots = _make_v2_plots(rows, results) if manifest.get("schema_version") == 2 else dict(zip(("plots/compute-cycles.svg", "plots/logical-payload-bytes.svg"), _make_plots(rows, results)))
    for rel, data in plots.items():
        store.write(rel, data, cap=caps[rel])
    store.write("host-diagnostics.jsonl", _jsonl_bytes(diagnostics), cap=caps["host-diagnostics.jsonl"])
    _finalize_manifest(store)


def _finish_v2_terminal_preflight(store: ArtifactStore, manifest: dict[str, Any],
                                 rows: list[dict[str, Any]], results: dict[int, dict[str, Any]],
                                 status: str, study_run_id: str,
                                 diagnostics: list[dict[str, Any]], phase_start_ns: int,
                                 sequence: int, failure_index: int | None = None) -> str:
    elapsed_ns = _refresh_v2_preflight_elapsed(store, diagnostics, phase_start_ns, sequence)
    if elapsed_ns > V2_LIMITS["max_active_seconds"] * 1_000_000_000:
        status = "limit_reached"
        preflight, _ = read_json(store.root / "preflight.json", max_bytes=V2_FILE_CAPS["preflight.json"])
        preflight["status"] = status
        preflight["error_codes"] = ["limit_reached"]
        store.write_json("preflight.json", preflight, cap=V2_FILE_CAPS["preflight.json"])
        _write_terminal_preflight_bundle(store, manifest, rows, results, status,
                                         study_run_id, diagnostics, failure_index)
        _refresh_v2_preflight_elapsed(store, diagnostics, phase_start_ns, sequence)
        _finalize_manifest(store)
    else:
        _finalize_manifest(store)
    verify_bundle(store.root)
    return status


def _row_to_ledger(row: dict[str, Any], c: dict[str, Any] | None,
                   execution_order: int | None = None) -> dict[str, Any]:
    if row["duplicate_of"] is not None:
        return _ledger_record(row, "duplicate", candidate_pair_id=row["candidate_pair_id"], error_codes=[], artifacts=[])
    if c is None:
        return _ledger_record(row, "not_started_due_to_limit", candidate_pair_id=row["candidate_pair_id"], error_codes=["not_started"])
    return _ledger_record(
        row, c["runner_status"], execution_order=c.get("execution_order"),
        process_exit=c.get("process_exit_code"), cpp_status=c.get("cpp_status"),
        rvv=c.get("rvv_arm_status"), matrix=c.get("matrix_arm_status"),
        candidate_pair_id=c.get("candidate_pair_id"), error_codes=c.get("error_codes"),
        error_paths=c.get("error_paths"), artifacts=c.get("artifact_paths"),
    )


def _study_status(rows: list[dict[str, Any]], results: dict[int, dict[str, Any]], global_failure: str | None = None) -> str:
    if global_failure:
        return global_failure
    for row in rows:
        if row["duplicate_of"] is not None:
            c = results.get(row["duplicate_of"])
            if c is None or c["runner_status"] != "complete":
                return "incomplete"
        else:
            c = results.get(row["source_row_index"])
            if c is None:
                return "limit_reached"
            if c["runner_status"] != "complete":
                return "incomplete"
    return "complete"


def _append_host_diagnostics(store: ArtifactStore, root: Path, records: list[dict[str, Any]], new_records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    combined = list(records) + list(new_records)
    store.write("host-diagnostics.jsonl", _jsonl_bytes(combined), cap=FILE_CAPS["host-diagnostics.jsonl"])
    return combined


def command_execute(args: argparse.Namespace) -> int:
    execute_start_ns = time.monotonic_ns()
    root_arg = Path(args.output_dir).expanduser()
    if root_arg.is_symlink():
        _fail("unsafe_path", "output root must not be a symlink", exit_code=4)
    root = root_arg.resolve(strict=True)
    store = ArtifactStore(root)
    manifest, raw_manifest, manifest_id = _load_runner_manifest(Path(args.manifest).expanduser())
    limits = _limits_for_manifest(manifest)
    caps = _caps_for_manifest(manifest)
    plan_obj, _, rows = _read_phase_artifacts_from_manifest(manifest, manifest_id, root)
    pilots = _pilot_candidates(rows, manifest)
    expected = _expected_preflight_paths(rows, pilots)
    _ensure_expected_files(root, expected)
    preflight, _ = read_json(root / "preflight.json", max_bytes=caps["preflight.json"])
    expected_pf_keys = {
        "schema_id", "schema_version", "manifest_id", "plan_sha256", "study_run_id",
        "executable_sha256", "status", "source_row_count", "unique_candidate_count",
        "exact_request_file_bytes", "reserved_peak_bytes", "worst_case_evaluator_seconds",
        "pilot_row_ids", "error_codes",
    }
    if not _has_schema_v1(preflight, "axissim.gemm-pair-study.preflight") or set(preflight) != expected_pf_keys:
        _fail("provenance_failed", "preflight contract has an invalid schema", exit_code=4)
    if preflight.get("status") != "ready":
        _fail("pilot_failed", "execute requires a ready preflight", exit_code=4)
    provenance, _ = read_json(root / "provenance.json", max_bytes=FILE_CAPS["provenance.json"])
    binary = _safe_binary(args.binary)
    identities = build_provenance(binary, Path(__file__).resolve(), Path(__file__).resolve().parents[1], manifest_id, rows)
    if identities["executable_sha256"] != preflight.get("executable_sha256") or identities["study_run_id"] != preflight.get("study_run_id") or identities["runner_source_sha256"] != provenance.get("runner_source_sha256"):
        _fail("provenance_failed", "runner, executable, or checkout identity changed since preflight", exit_code=4)
    request_bytes, _, peak_bytes = _compute_storage_bound(root, rows, len(rows))
    expected_pilot_ids = [row["study_row_id"] for row in pilots]
    expected_pf = {
        "manifest_id": manifest_id,
        "plan_sha256": sha256_bytes(canonical_bytes(plan_obj)),
        "study_run_id": identities["study_run_id"],
        "executable_sha256": identities["executable_sha256"],
        "status": "ready",
        "source_row_count": len(rows),
        "unique_candidate_count": len({r["request_bytes"] for r in rows}),
        "exact_request_file_bytes": request_bytes,
        "reserved_peak_bytes": peak_bytes,
        "worst_case_evaluator_seconds": len({r["request_bytes"] for r in rows}) * limits["max_evaluator_seconds"],
        "pilot_row_ids": expected_pilot_ids,
        "error_codes": [],
    }
    if any(preflight.get(key) != value for key, value in expected_pf.items()):
        _fail("provenance_failed", "preflight values do not match the regenerated study plan", exit_code=4)
    expected_provenance_keys = {
        "schema_id", "schema_version", "manifest_id", "study_label", "study_run_id",
        "manifest_input_sha256", "runner_source_sha256", "source_checkout_id", "source_revision",
        "source_tree_state", "dirty_diff_sha256", "executable_sha256", "cpp_simulator_identity",
        "request_sha256s",
    }
    if not _has_schema_v1(provenance, "axissim.gemm-pair-study.provenance") or set(provenance) != expected_provenance_keys:
        _fail("provenance_failed", "provenance contract has an invalid schema", exit_code=4)
    provenance_expected = {
        "manifest_id": manifest_id,
        "study_label": manifest["study_label"],
        "study_run_id": identities["study_run_id"],
        "manifest_input_sha256": sha256_bytes(raw_manifest),
        "runner_source_sha256": identities["runner_source_sha256"],
        "source_checkout_id": identities["source_checkout_id"],
        "source_revision": identities["source_revision"],
        "source_tree_state": identities["source_tree_state"],
        "dirty_diff_sha256": identities["dirty_diff_sha256"],
        "executable_sha256": identities["executable_sha256"],
        "request_sha256s": identities["request_sha256s"],
    }
    if any(provenance.get(key) != value for key, value in provenance_expected.items()):
        _fail("provenance_failed", "provenance fields differ from current inputs", exit_code=4)
    if preflight.get("plan_sha256") != sha256_bytes(canonical_bytes(plan_obj)):
        _fail("provenance_failed", "plan digest differs from preflight", exit_code=4)
    previous_host, _ = read_jsonl(root / "host-diagnostics.jsonl")
    preflight_elapsed = sum(x.get("phase_elapsed_ns", 0) for x in previous_host if x.get("record_type") == "phase" and x.get("phase") == "preflight")
    results: dict[int, dict[str, Any]] = {}
    # Revalidate the three pilot artifacts and reuse them without another invocation.
    for order, row in enumerate(pilots):
        raw = (root / row["result_path"]).read_bytes()
        response, response_raw, parse_error = _cpp_response(root / row["result_path"], row.get("result_payload_cap"))
        synthetic_proc = {"process_exit_code": 0, "timed_out": False, "launch_error": False}
        classified = classify_cpp_response(response, response_raw, parse_error, synthetic_proc, row, identities["executable_sha256"])
        if classified["runner_status"] != "complete":
            _fail("provenance_failed", "a pilot result no longer validates as complete", exit_code=4)
        classified["execution_order"] = order
        classified["process_exit_code"] = 0
        classified["artifact_paths"] = [row["result_path"]]
        results[row["source_row_index"]] = classified
    host_records = list(previous_host)
    evaluator_seq = sum(1 for x in host_records if x.get("record_type") == "evaluator")
    launched_order = len(pilots)
    global_failure: str | None = None
    unique_seen: set[bytes] = {row["request_bytes"] for row in pilots}
    for row in rows:
        if row["duplicate_of"] is not None or row["request_bytes"] in unique_seen:
            if row["duplicate_of"] is None:
                unique_seen.add(row["request_bytes"])
            continue
        elapsed = preflight_elapsed / 1_000_000_000 + (time.monotonic_ns() - execute_start_ns) / 1_000_000_000
        remaining = limits["max_active_seconds"] - elapsed
        if remaining <= 0:
            global_failure = "limit_reached"
            break
        timeout = min(float(limits["max_evaluator_seconds"]), remaining)
        proc, response, raw, parse_error = _invoke_candidate(binary, root, row, timeout)
        diag = _evaluator_diag("execute", evaluator_seq, row, proc)
        evaluator_seq += 1
        _append_host_diagnostics(store, root, host_records, [diag])
        host_records.append(diag)
        classified = classify_cpp_response(response, raw, parse_error, proc, row, identities["executable_sha256"])
        classified["execution_order"] = launched_order
        classified["process_exit_code"] = proc.get("process_exit_code")
        launched_order += 1
        if raw is not None:
            artifacts = _store_identityless_request(store, row, classified)
            artifacts.extend(_candidate_result_artifact(store, row, classified))
            classified["artifact_paths"] = artifacts
        else:
            classified["artifact_paths"] = []
        results[row["source_row_index"]] = classified
        unique_seen.add(row["request_bytes"])
        if proc.get("cancelled"):
            global_failure = "limit_reached"
            break
        if proc.get("timed_out") and remaining <= timeout:
            global_failure = "limit_reached"
            break
    for row in rows:
        if row["duplicate_of"] is not None:
            continue
        if row["source_row_index"] not in results:
            status = "not_started_due_to_limit" if global_failure == "limit_reached" else "not_started_provenance_failure" if global_failure == "provenance_failed" else "not_started_due_to_limit"
            results[row["source_row_index"]] = {
                "runner_status": status, "error_codes": [status], "error_paths": [], "artifact_paths": [],
                "process_exit_code": None, "candidate_pair_id": row["candidate_pair_id"],
            }
    if (manifest.get("schema_version") == 2 and
            preflight_elapsed + time.monotonic_ns() - execute_start_ns > limits["max_active_seconds"] * 1_000_000_000):
        global_failure = "limit_reached"
    study_status = _study_status(rows, results, global_failure)
    ledger = [_row_to_ledger(row, results.get(row["source_row_index"])) for row in rows]
    store.write_jsonl("runner-ledger.jsonl", ledger, cap=caps["runner-ledger.jsonl"], row_cap=2048)
    summary = _build_summary(manifest, rows, results, study_status, identities["study_run_id"])
    store.write_json("summary.json", summary, cap=caps["summary.json"])
    csv_data = _summary_csv(rows, results, summary["rows"]).encode("ascii")
    store.write("summary.csv", csv_data, cap=caps["summary.csv"])
    report = _make_report(manifest, rows, results, study_status).encode("utf-8")
    store.write("report.md", report, cap=caps["report.md"])
    if manifest.get("schema_version") == 2:
        for rel, data in _make_v2_plots(rows, results).items():
            store.write(rel, data, cap=caps[rel])
    else:
        plot_cycles, plot_traffic = _make_plots(rows, results)
        store.write("plots/compute-cycles.svg", plot_cycles, cap=caps["plots/compute-cycles.svg"])
        store.write("plots/logical-payload-bytes.svg", plot_traffic, cap=caps["plots/logical-payload-bytes.svg"])
    if manifest.get("schema_version") == 2:
        active_elapsed_ns = preflight_elapsed + time.monotonic_ns() - execute_start_ns
        if active_elapsed_ns > limits["max_active_seconds"] * 1_000_000_000 and study_status != "limit_reached":
            study_status = "limit_reached"
            summary = _build_summary(manifest, rows, results, study_status, identities["study_run_id"])
            store.write_json("summary.json", summary, cap=caps["summary.json"])
            store.write("report.md", _make_report(manifest, rows, results, study_status).encode("utf-8"), cap=caps["report.md"])
        host_records.append(_phase_diag("execute", evaluator_seq, time.monotonic_ns() - execute_start_ns))
        _append_host_diagnostics(store, root, host_records[:-1], [host_records[-1]])
        _finalize_manifest(store)
        verify_bundle(root)
        # Include artifact publication and the first integrity pass. The final
        # deterministic hash-seal write is bounded by the 2 MiB bundle cap.
        active_elapsed_ns = preflight_elapsed + time.monotonic_ns() - execute_start_ns
        if active_elapsed_ns > limits["max_active_seconds"] * 1_000_000_000 and study_status != "limit_reached":
            study_status = "limit_reached"
            summary = _build_summary(manifest, rows, results, study_status, identities["study_run_id"])
            store.write_json("summary.json", summary, cap=caps["summary.json"])
            store.write("report.md", _make_report(manifest, rows, results, study_status).encode("utf-8"), cap=caps["report.md"])
        host_records[-1] = _phase_diag("execute", evaluator_seq, time.monotonic_ns() - execute_start_ns)
        _append_host_diagnostics(store, root, host_records[:-1], [host_records[-1]])
        _finalize_manifest(store)
        verify_bundle(root)
        # The second seal/verification can itself cross the active-time limit.
        # Check after it as well, and publish a verified limit_reached bundle
        # rather than leaving a completed status on an over-budget run.
        active_elapsed_ns = preflight_elapsed + time.monotonic_ns() - execute_start_ns
        if active_elapsed_ns > limits["max_active_seconds"] * 1_000_000_000 and study_status != "limit_reached":
            study_status = "limit_reached"
            summary = _build_summary(manifest, rows, results, study_status, identities["study_run_id"])
            store.write_json("summary.json", summary, cap=caps["summary.json"])
            store.write("report.md", _make_report(manifest, rows, results, study_status).encode("utf-8"), cap=caps["report.md"])
            host_records[-1] = _phase_diag("execute", evaluator_seq, time.monotonic_ns() - execute_start_ns)
            _append_host_diagnostics(store, root, host_records[:-1], [host_records[-1]])
            _finalize_manifest(store)
            verify_bundle(root)
        elapsed_ns = time.monotonic_ns() - execute_start_ns
    else:
        _finalize_manifest(store)
        verify_bundle(root)
        elapsed_ns = time.monotonic_ns() - execute_start_ns
    print(f"Study status: {study_status}")
    print(f"Completed pairs: {sum(1 for r in results.values() if r.get('runner_status') == 'complete')}")
    print(f"Actual evaluator invocations: {evaluator_seq}; actual active seconds: {(preflight_elapsed + elapsed_ns) / 1e9:.6f}")
    print(f"Preflight evaluator upper bound: {preflight.get('worst_case_evaluator_seconds')} seconds")
    return 0 if study_status == "complete" else (5 if global_failure == "output_limit" else 4 if global_failure == "provenance_failed" else 3)


def read_jsonl(path: Path) -> tuple[list[dict[str, Any]], bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        _fail("io_error", f"cannot read {path.name}: {exc}", exit_code=4)
    if len(raw) > FILE_CAPS["host-diagnostics.jsonl"]:
        _fail("output_limit", "JSONL artifact exceeds cap", exit_code=5)
    result = []
    for line in raw.splitlines():
        if not line:
            continue
        value = parse_json(line)
        if not isinstance(value, dict):
            _fail("result_schema_error", "JSONL record is not an object", exit_code=4)
        result.append(value)
    return result, raw


def _render_artifact_manifest(root: Path) -> dict[str, Any]:
    excluded = {"canonical-artifact-manifest.json", "host-diagnostics.jsonl", "bundle-index.json"}
    artifacts = []
    for rel in sorted(_actual_file_paths(root)):
        if rel in excluded:
            continue
        path = root / rel
        raw = path.read_bytes()
        artifacts.append({"path": rel, "size_bytes": len(raw), "sha256": sha256_bytes(raw)})
    return {"schema_id": "axissim.gemm-pair-study.artifacts", "schema_version": 1, "artifacts": artifacts}


def _finalize_manifest(store: ArtifactStore) -> None:
    root = store.root
    artifact_manifest = _render_artifact_manifest(root)
    store.write_json("canonical-artifact-manifest.json", artifact_manifest, cap=FILE_CAPS["canonical-artifact-manifest.json"])
    raw_artifact_manifest = (root / "canonical-artifact-manifest.json").read_bytes()
    raw_diag = (root / "host-diagnostics.jsonl").read_bytes() if (root / "host-diagnostics.jsonl").exists() else b""
    index = {
        "schema_id": "axissim.gemm-pair-study.bundle-index",
        "schema_version": 1,
        "canonical_artifact_manifest_sha256": sha256_bytes(raw_artifact_manifest),
        "host_diagnostics_sha256": sha256_bytes(raw_diag),
    }
    store.write_json("bundle-index.json", index, cap=FILE_CAPS["bundle-index.json"])


def _check_path_set(root: Path, artifact_manifest: dict[str, Any], schema_version: int = 1) -> None:
    limits = V2_LIMITS if schema_version == 2 else FIXED_LIMITS
    caps = V2_FILE_CAPS if schema_version == 2 else FILE_CAPS
    entries = artifact_manifest.get("artifacts")
    if not isinstance(entries, list):
        _fail("integrity_failed", "artifact manifest lacks an artifact list", exit_code=4)
    listed: set[str] = set()
    previous = ""
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "size_bytes", "sha256"}:
            _fail("integrity_failed", "artifact manifest entry has wrong schema", exit_code=4)
        rel = entry["path"]
        _safe_rel(rel)
        if isinstance(entry["size_bytes"], bool) or not isinstance(entry["size_bytes"], int) or entry["size_bytes"] < 0 or not _is_sha256(entry["sha256"]):
            _fail("integrity_failed", "artifact manifest size/hash fields are malformed", exit_code=4)
        if rel <= previous or rel in listed:
            _fail("integrity_failed", "artifact manifest paths are not unique sorted paths", exit_code=4)
        previous = rel
        listed.add(rel)
        file = _check_no_symlink(root, rel)
        if not file.is_file():
            _fail("integrity_failed", f"missing artifact: {rel}", exit_code=4)
        data = file.read_bytes()
        if len(data) != entry["size_bytes"] or sha256_bytes(data) != entry["sha256"]:
            _fail("integrity_failed", f"artifact hash/size mismatch: {rel}", exit_code=4)
        if rel.startswith("requests/") and len(data) > limits["max_request_file_bytes"]:
            _fail("integrity_failed", "request file exceeds the C++ input cap", exit_code=4)
        if rel.startswith("results/") and len(data) > limits["max_result_payload_bytes"] + 1:
            _fail("integrity_failed", "result file exceeds the C++ output cap", exit_code=4)
        if rel in caps and len(data) > caps[rel]:
            _fail("integrity_failed", f"artifact exceeds its per-file cap: {rel}", exit_code=4)
    expected = listed | {"canonical-artifact-manifest.json", "host-diagnostics.jsonl", "bundle-index.json"}
    actual = _actual_file_paths(root)
    if actual != expected:
        _fail("integrity_failed", f"unexpected artifact paths: extra={sorted(actual-expected)}, missing={sorted(expected-actual)}", exit_code=4)
    staging = root / ".staging"
    if staging.exists() and any(staging.iterdir()):
        _fail("integrity_failed", "staging directory contains unfinished files", exit_code=4)
    if file_bytes_under(root) > limits["max_bundle_bytes"]:
        _fail("integrity_failed", "bundle exceeds the 2 MiB total file-byte limit", exit_code=4)


def _is_sha256(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _has_schema_v1(record: Any, schema_id: str) -> bool:
    return (isinstance(record, dict) and record.get("schema_id") == schema_id and
            type(record.get("schema_version")) is int and record["schema_version"] == 1)


def _is_exact_index(value: Any, expected: int) -> bool:
    return type(value) is int and value == expected


def _verify_plan_record(plan: Any, raw: bytes, expected: dict[str, Any]) -> None:
    if (not isinstance(plan, dict) or plan.get("schema_id") != "axissim.gemm-pair-study.plan" or
            type(plan.get("schema_version")) is not int or plan.get("schema_version") != expected.get("schema_version") or
            plan != expected or raw != canonical_bytes(expected) + b"\n"):
        _fail("integrity_failed", "plan schema, identity, or candidate ordering is invalid", exit_code=4)


def _verify_cpp_error_envelope(error: Any, expected_status: str,
                               row: dict[str, Any]) -> dict[str, Any]:
    common = {"schema_id", "schema_version", "status", "errors"}
    identity_keys = {"candidate_pair_id", "request_sha256", "model_assumptions_sha256"}
    expected_keys = common | (identity_keys if expected_status == "output_limit" else set())
    if (not _has_schema_v1(error, "axissim.gemm-pair.error") or set(error) != expected_keys or
            error.get("status") != expected_status):
        _fail("integrity_failed", "C++ error envelope has an invalid schema or status", exit_code=4)
    if expected_status == "output_limit":
        ids = row["ids"]
        if any(error.get(field) != ids[identity] for field, identity in (
            ("candidate_pair_id", "candidate_pair_id"),
            ("request_sha256", "request_sha256"),
            ("model_assumptions_sha256", "model_assumptions_sha256"),
        )):
            _fail("integrity_failed", "output-limit envelope identity differs from its planned request", exit_code=4)
    errors = error.get("errors")
    if not isinstance(errors, list) or len(errors) != 1 or not isinstance(errors[0], dict) or set(errors[0]) != {"scope", "code", "path"}:
        _fail("integrity_failed", "C++ error envelope must contain exactly one structured error", exit_code=4)
    item = errors[0]
    if not all(isinstance(item.get(field), str) and item[field].isascii() for field in ("scope", "code", "path")):
        _fail("integrity_failed", "C++ error scope, code, and path must be ASCII strings", exit_code=4)
    if expected_status in {"invalid_json", "input_limit", "unsupported_schema", "invalid_request"}:
        if item["scope"] != "request" or (item["path"] and not item["path"].startswith("/")):
            _fail("integrity_failed", "invalid-request error scope or pointer is malformed", exit_code=4)
    elif expected_status == "internal_error":
        if item != {"scope": "global", "code": "internal_error", "path": ""}:
            _fail("integrity_failed", "internal-error envelope has an unexpected error record", exit_code=4)
    elif expected_status == "output_limit":
        if item != {"scope": "global", "code": "output_limit", "path": ""}:
            _fail("integrity_failed", "output-limit envelope has an unexpected error record", exit_code=4)
    else:
        _fail("integrity_failed", "unsupported C++ error-envelope status", exit_code=4)
    return item


def _verify_terminal_row_fields(status: str, led: dict[str, Any], row: dict[str, Any]) -> None:
    code = led.get("process_exit_code")
    candidate = led.get("candidate_pair_id")
    artifacts = led["artifact_paths"]
    codes = led["error_codes"]
    expected_result = row["result_path"]
    identityless_error = f"results/{row['study_row_id']}.error.json"
    identityless_request = f"requests/{row['study_row_id']}.request.json"

    def reject(message: str) -> None:
        _fail("integrity_failed", f"{status} terminal mapping is inconsistent: {message}", exit_code=4)

    if "process_exit_code" in led and (
        isinstance(code, bool) or not isinstance(code, int) or not 0 <= code <= MAX_SAFE
    ):
        reject("process exit code must be a nonnegative normalized integer")

    if status == "duplicate":
        if candidate != row["candidate_pair_id"] or artifacts or codes or code is not None or "execution_order" in led:
            reject("duplicate rows cannot launch a process or own artifacts")
    elif status in {"not_started_due_to_limit", "not_started_provenance_failure", "not_started_due_to_preflight_failure"}:
        allowed_codes = {
            "not_started_due_to_limit": {"not_started_due_to_limit", "output_limit", "active_time_limit"},
            "not_started_provenance_failure": {"not_started_provenance_failure", "provenance_failed"},
            "not_started_due_to_preflight_failure": {"pilot_failed", "pilot_not_run"},
        }[status]
        if candidate != row["candidate_pair_id"] or code is not None or artifacts or len(codes) != 1 or codes[0] not in allowed_codes or any(field in led for field in ("execution_order", "cpp_status", "rvv_arm_status", "matrix_arm_status")):
            reject("unstarted row must have a stable reason and no process result")
    elif status == "timeout":
        if candidate is not None or code in (None, 0) or codes not in (["evaluator_timeout"], ["evaluator_cancelled"]) or artifacts or any(field in led for field in ("cpp_status", "rvv_arm_status", "matrix_arm_status")) or "execution_order" not in led:
            reject("timeout requires a terminated process record and no candidate result")
    elif status == "missing_result":
        if candidate is not None or code in (None, 2) or codes != ["missing_result"] or artifacts or any(field in led for field in ("cpp_status", "rvv_arm_status", "matrix_arm_status")) or "execution_order" not in led:
            reject("missing result must have no parsed candidate data or result artifact")
    elif status == "process_error":
        if codes not in (["process_launch_failed"], ["cli_usage_no_result"], ["cpp_status_exit_mismatch"]):
            reject("process error has an unknown stable error code")
        if "execution_order" not in led:
            reject("process error must correspond to an ordered launch attempt")
        if codes == ["process_launch_failed"] and (code is not None or artifacts or candidate is not None):
            reject("launch failure cannot have an exit code or result artifacts")
        if codes == ["cli_usage_no_result"] and (code != 2 or artifacts):
            reject("exit-2 usage failure cannot have a result artifact")
        if codes == ["cpp_status_exit_mismatch"] and (code is None or artifacts not in ([], [expected_result], [identityless_error], [identityless_request, identityless_error])):
            reject("exit/status mismatch has an invalid process result pattern")
    elif status == "invalid":
        if code != 2 or candidate is not None or led.get("cpp_status") not in {"invalid_json", "input_limit", "unsupported_schema", "invalid_request"} or artifacts != [identityless_request, identityless_error] or not codes or "execution_order" not in led or any(field in led for field in ("rvv_arm_status", "matrix_arm_status")):
            reject("invalid request must preserve its request and C++ error envelope")
    elif status == "partial":
        if (code != 3 or candidate != row["candidate_pair_id"] or led.get("cpp_status") != "partial" or
                artifacts != [expected_result] or not codes or "execution_order" not in led or
                any(field not in led for field in ("rvv_arm_status", "matrix_arm_status"))):
            reject("partial result must retain the paired C++ record and error codes")
    elif status == "failed":
        if code != 4 or "execution_order" not in led:
            reject("failed result requires a C++ exit-4 process record")
        if candidate == row["candidate_pair_id"]:
            if led.get("cpp_status") != "failed" or artifacts != [expected_result] or not codes:
                reject("candidate failure must retain its paired C++ result")
        elif candidate is None:
            if led.get("cpp_status") != "internal_error" or artifacts not in ([identityless_error], [identityless_request, identityless_error]) or codes != ["internal_error"]:
                reject("identity-less internal error must retain its row-scoped envelope")
        else:
            reject("failed result has an unexpected candidate identity")
    elif status == "complete":
        if (code != 0 or candidate != row["candidate_pair_id"] or led.get("cpp_status") != "complete" or
                led.get("rvv_arm_status") != "complete" or led.get("matrix_arm_status") != "complete" or
                artifacts != [expected_result] or codes or led.get("error_paths", []) or "execution_order" not in led):
            reject("complete result must retain a successful paired C++ record with no errors")
    elif status == "identity_mismatch":
        if code is None or artifacts != [expected_result] or "execution_order" not in led or "identity_mismatch" not in codes:
            reject("identity mismatch must retain the candidate result that failed identity checks")
        if led.get("cpp_status") not in {"complete", "partial", "failed"}:
            reject("identity mismatch has no recognized C++ candidate status")
    elif status == "result_schema_error":
        if code is None or not codes or "execution_order" not in led or artifacts not in ([expected_result], [identityless_error], [identityless_request, identityless_error]):
            reject("schema error must retain the malformed result or error artifact")
    elif status == "output_limit":
        if code is None or not codes or "execution_order" not in led:
            reject("output limit must identify the process that exceeded a limit")
        if "result_output_limit" in codes:
            if (codes != ["result_output_limit"] or artifacts or candidate is not None or
                    any(field in led for field in ("cpp_status", "rvv_arm_status", "matrix_arm_status")) or
                    led.get("error_paths", []) != []):
                reject("runner result-size limit cannot claim candidate or C++ result fields")
        elif led.get("cpp_status") == "output_limit" and code == 5:
            if artifacts not in ([expected_result], [identityless_error], [identityless_request, identityless_error]):
                reject("C++ output-limit envelope has an invalid artifact path")
        else:
            reject("output-limit status lacks the expected C++ or runner output-limit code")


def _verify_result_ledger_consistency(status: str, led: dict[str, Any], result: dict[str, Any],
                                      row: dict[str, Any]) -> None:
    expected_exit = {"complete": 0, "partial": 3, "failed": 4}.get(status)
    if expected_exit is None or result.get("status") != status:
        _fail("integrity_failed", "candidate result status differs from its ledger status", exit_code=4)
    if (led.get("process_exit_code") != expected_exit or
            led.get("candidate_pair_id") != row["candidate_pair_id"] or
            led.get("cpp_status") != result.get("status") or
            led.get("rvv_arm_status") != result.get("arms", {}).get("rvv", {}).get("arm_status") or
            led.get("matrix_arm_status") != result.get("arms", {}).get("matrix", {}).get("arm_status") or
            led.get("error_codes") != _error_codes(result) or
            led.get("error_paths", []) != _error_paths(result)):
        _fail("integrity_failed", "candidate result status/errors differ from its ledger fields", exit_code=4)


def _v2_launch_order_matches(rows: list[dict[str, Any]], ledger: list[dict[str, Any]]) -> bool:
    pilots = [row for row in rows if row["capacity_level_id"] == "p4"]
    remaining = [row for row in rows if row["capacity_level_id"] != "p4"]
    planned = [row["source_row_index"] for row in pilots + remaining]
    launches = [
        (led["execution_order"], row["source_row_index"])
        for row, led in zip(rows, ledger) if "execution_order" in led
    ]
    launched = [row_index for _, row_index in sorted(launches, key=lambda item: item[0])]
    return launched == planned[:len(launched)]


def _verify_host_diagnostics(records: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    common = {"schema_id", "schema_version", "record_type", "phase", "sequence", "python_version", "os_id", "architecture_id"}
    phase_keys = common | {"phase_elapsed_ns"}
    evaluator_keys = common | {"study_row_id", "evaluator_elapsed_ns", "stdout_bytes", "stdout_sha256", "stderr_bytes", "stderr_sha256"}
    phase_seen: set[str] = set()
    evaluator_sequences: list[int] = []
    evaluator_by_row: dict[str, dict[str, Any]] = {}
    for record in records:
        if not _has_schema_v1(record, "axissim.gemm-pair-study.host-diagnostic"):
            _fail("integrity_failed", "host diagnostic record has an unsupported schema", exit_code=4)
        kind = record.get("record_type")
        optional = {"process_exit_code"} if kind == "evaluator" and "process_exit_code" in record else set()
        expected = (phase_keys if kind == "phase" else evaluator_keys | optional if kind == "evaluator" else set())
        if set(record) != expected or record.get("phase") not in {"preflight", "execute"}:
            _fail("integrity_failed", "host diagnostic record has an invalid field set", exit_code=4)
        for field in ("python_version", "os_id", "architecture_id"):
            if not isinstance(record.get(field), str) or not record[field].isascii():
                _fail("integrity_failed", "host diagnostic identity field is malformed", exit_code=4)
        sequence = record.get("sequence")
        if isinstance(sequence, bool) or not isinstance(sequence, int) or not 0 <= sequence <= MAX_SAFE:
            _fail("integrity_failed", "host diagnostic sequence is invalid", exit_code=4)
        if kind == "phase":
            phase = record["phase"]
            if phase in phase_seen:
                _fail("integrity_failed", "duplicate host phase diagnostic", exit_code=4)
            phase_seen.add(phase)
            duration = record.get("phase_elapsed_ns")
            if isinstance(duration, bool) or not isinstance(duration, int) or not 0 <= duration <= MAX_SAFE:
                _fail("integrity_failed", "host phase duration is invalid", exit_code=4)
        else:
            evaluator_sequences.append(sequence)
            row_id = record.get("study_row_id")
            if not _is_sha256(row_id) or row_id in evaluator_by_row:
                _fail("integrity_failed", "host evaluator diagnostic lacks a valid row identity", exit_code=4)
            if "process_exit_code" in record and (isinstance(record["process_exit_code"], bool) or not isinstance(record["process_exit_code"], int) or not 0 <= record["process_exit_code"] <= MAX_SAFE):
                _fail("integrity_failed", "host evaluator process status is invalid", exit_code=4)
            for field in ("evaluator_elapsed_ns", "stdout_bytes", "stderr_bytes"):
                value = record.get(field)
                if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= MAX_SAFE:
                    _fail("integrity_failed", "host evaluator count or duration is invalid", exit_code=4)
            if not _is_sha256(record.get("stdout_sha256")) or not _is_sha256(record.get("stderr_sha256")):
                _fail("integrity_failed", "host evaluator stream hash is invalid", exit_code=4)
            evaluator_by_row[row_id] = record
    if "preflight" not in phase_seen or sorted(evaluator_sequences) != list(range(len(evaluator_sequences))):
        _fail("integrity_failed", "host diagnostic phases or launch sequence are incomplete", exit_code=4)
    return evaluator_by_row


def _verify_ledger_host_launch(study_row_id: str, led: dict[str, Any],
                              evaluator_by_row: dict[str, dict[str, Any]]) -> None:
    if "execution_order" in led:
        value = led["execution_order"]
        diagnostic = evaluator_by_row.get(study_row_id)
        if (isinstance(value, bool) or not isinstance(value, int) or value < 0 or
                diagnostic is None or diagnostic.get("sequence") != value):
            _fail("integrity_failed", "ledger launch order has no matching host diagnostic", exit_code=4)
        ledger_has_exit = "process_exit_code" in led
        diagnostic_has_exit = "process_exit_code" in diagnostic
        if ledger_has_exit != diagnostic_has_exit or (
            ledger_has_exit and led["process_exit_code"] != diagnostic["process_exit_code"]
        ):
            _fail("integrity_failed", "ledger and host diagnostic process statuses disagree", exit_code=4)
    elif study_row_id in evaluator_by_row:
        _fail("integrity_failed", "host diagnostic has no corresponding ledger launch", exit_code=4)


def _verify_diagnostic_ledger_set(ledger: list[dict[str, Any]],
                                  evaluator_by_row: dict[str, dict[str, Any]]) -> None:
    launched_rows = {led.get("study_row_id") for led in ledger if "execution_order" in led}
    if launched_rows != set(evaluator_by_row):
        _fail("integrity_failed", "host evaluator diagnostics and launched ledger rows differ", exit_code=4)


def _verify_bundle_contract_v2(root: Path, summary: dict[str, Any], ledger: list[dict[str, Any]],
                              evaluator_by_row: dict[str, dict[str, Any]]) -> None:
    required = {
        "manifest.json", "plan.json", "preflight.json", "provenance.json", "runner-ledger.jsonl",
        "summary.json", "summary.csv", "report.md", "plots/compute-cycles.svg",
        "plots/logical-payload-bytes.svg", "plots/serialized-total-cycles.svg",
        "plots/serialized-cycle-breakdown.svg", "canonical-artifact-manifest.json",
        "host-diagnostics.jsonl", "bundle-index.json",
    }
    paths = _actual_file_paths(root)
    if not required.issubset(paths):
        _fail("integrity_failed", "v2 bundle is missing required report files", exit_code=4)
    manifest_obj, manifest_raw = read_json(root / "manifest.json", max_bytes=V2_FILE_CAPS["manifest.json"])
    manifest = validate_manifest(manifest_obj)
    if manifest["schema_version"] != 2 or manifest_raw != canonical_bytes(manifest) + b"\n":
        _fail("integrity_failed", "v2 manifest is not canonical", exit_code=4)
    manifest_id = canonical_hash(manifest)
    rows = expand_candidates(manifest, manifest_id)
    plan, plan_raw = read_json(root / "plan.json", max_bytes=V2_FILE_CAPS["plan.json"])
    _verify_plan_record(plan, plan_raw, make_plan(manifest, manifest_id, rows))
    for row in rows:
        path = root / row["request_path"]
        if not path.is_file() or path.read_bytes() != row["request_bytes"]:
            _fail("integrity_failed", "v2 canonical request is missing or changed", exit_code=4)
    request_paths = {row["request_path"] for row in rows}

    preflight, _ = read_json(root / "preflight.json", max_bytes=V2_FILE_CAPS["preflight.json"])
    pf_keys = {
        "schema_id", "schema_version", "manifest_id", "plan_sha256", "study_run_id", "executable_sha256",
        "status", "source_row_count", "unique_candidate_count", "exact_request_file_bytes", "reserved_peak_bytes",
        "worst_case_evaluator_seconds", "pilot_row_ids", "error_codes",
    }
    if not _has_schema_v1(preflight, "axissim.gemm-pair-study.preflight") or set(preflight) != pf_keys:
        _fail("integrity_failed", "v2 preflight schema is invalid", exit_code=4)
    if preflight.get("status") not in {"ready", "pilot_failed", "limit_reached", "provenance_failed", "output_limit"}:
        _fail("integrity_failed", "v2 preflight status is unsupported", exit_code=4)
    request_bytes, _, peak_bytes = _compute_storage_bound(root, rows, len(rows))
    pilots = _pilot_candidates(rows, manifest)
    expected_pf = {
        "manifest_id": manifest_id, "plan_sha256": sha256_bytes(canonical_bytes(plan)),
        "source_row_count": 24, "unique_candidate_count": 24,
        "exact_request_file_bytes": request_bytes, "reserved_peak_bytes": peak_bytes,
        "worst_case_evaluator_seconds": 24 * V2_LIMITS["max_evaluator_seconds"],
        "pilot_row_ids": [row["study_row_id"] for row in pilots],
    }
    if any(preflight.get(k) != v for k, v in expected_pf.items()):
        _fail("integrity_failed", "v2 preflight does not match the regenerated 24-case design", exit_code=4)
    if peak_bytes > V2_LIMITS["max_bundle_bytes"]:
        _fail("integrity_failed", "v2 preflight exceeds the approved bundle cap", exit_code=4)

    provenance, _ = read_json(root / "provenance.json", max_bytes=V2_FILE_CAPS["provenance.json"])
    provenance_keys = {
        "schema_id", "schema_version", "manifest_id", "study_label", "study_run_id", "manifest_input_sha256",
        "runner_source_sha256", "source_checkout_id", "source_revision", "source_tree_state", "dirty_diff_sha256",
        "executable_sha256", "cpp_simulator_identity", "request_sha256s",
    }
    if not _has_schema_v1(provenance, "axissim.gemm-pair-study.provenance") or set(provenance) != provenance_keys:
        _fail("integrity_failed", "v2 provenance schema is invalid", exit_code=4)
    if (provenance.get("manifest_id") != manifest_id or provenance.get("study_label") != manifest["study_label"] or
            provenance.get("study_run_id") != preflight.get("study_run_id") or
            provenance.get("executable_sha256") != preflight.get("executable_sha256") or
            provenance.get("request_sha256s") != [row["request_sha256"] for row in rows]):
        _fail("integrity_failed", "v2 provenance identities differ from the plan", exit_code=4)
    for field in ("manifest_input_sha256", "runner_source_sha256", "source_checkout_id"):
        if not _is_sha256(provenance.get(field)) and not (preflight["status"] == "provenance_failed" and provenance.get(field) == "unknown"):
            _fail("integrity_failed", f"v2 provenance field {field} is invalid", exit_code=4)
    if preflight["status"] == "ready":
        if not _is_sha256(preflight.get("study_run_id")) or not _is_sha256(preflight.get("executable_sha256")) or preflight.get("error_codes") != []:
            _fail("integrity_failed", "ready v2 preflight lacks complete identities", exit_code=4)
    elif not isinstance(preflight.get("error_codes"), list) or not preflight["error_codes"]:
        _fail("integrity_failed", "failed v2 preflight needs a stable error code", exit_code=4)

    summary_keys = {
        "schema_id", "schema_version", "study_label", "study_run_id", "study_status", "counts", "rows",
        "baseline_reference", "sensitivity_cases", "hypotheses", "interpretation_scope",
    }
    if (not isinstance(summary, dict) or summary.get("schema_id") != "axissim.gemm-pair-study.summary" or
            type(summary.get("schema_version")) is not int or summary.get("schema_version") != 2 or set(summary) != summary_keys):
        _fail("integrity_failed", "v2 summary schema is invalid", exit_code=4)
    if summary.get("study_label") != manifest["study_label"] or summary.get("study_run_id") != preflight.get("study_run_id"):
        _fail("integrity_failed", "v2 summary identity differs from provenance", exit_code=4)
    if summary.get("baseline_reference") != manifest["baseline_reference"] or summary.get("sensitivity_cases") != manifest["sensitivity_cases"] or summary.get("interpretation_scope") != "deterministic_model_checks_and_declared_scenario_only":
        _fail("integrity_failed", "v2 summary assumptions differ from manifest", exit_code=4)
    if not isinstance(ledger, list) or len(ledger) != 24 or not isinstance(summary.get("rows"), list) or len(summary["rows"]) != 24:
        _fail("integrity_failed", "v2 ledger and summary must each account for all 24 rows", exit_code=4)
    counts = summary.get("counts")
    if not isinstance(counts, dict) or set(counts) != set(STATUS_VALUES) or any(type(value) is not int or value < 0 for value in counts.values()) or sum(counts.values()) != 24:
        _fail("integrity_failed", "v2 status counts are malformed", exit_code=4)
    if any(counts[status] != sum(item.get("runner_status") == status for item in summary["rows"]) for status in STATUS_VALUES):
        _fail("integrity_failed", "v2 status counts disagree with summary rows", exit_code=4)

    derived: dict[int, dict[str, Any]] = {}
    references: set[str] = set()
    orders: list[int] = []
    for index, (row, led, summ) in enumerate(zip(rows, ledger, summary["rows"])):
        if not isinstance(summ, dict):
            _fail("integrity_failed", "v2 summary row must be an object", exit_code=4)
        core = {"schema_id", "schema_version", "study_row_id", "comparison_group_id", "source_row_index", "runner_status", "error_codes", "error_paths", "artifact_paths", "base_case_id", "sensitivity_case_id", "request_sha256"}
        optional = {"execution_order", "process_exit_code", "candidate_pair_id", "cpp_status", "rvv_arm_status", "matrix_arm_status"}
        if not isinstance(led, dict) or not core.issubset(led) or not set(led).issubset(core | optional):
            _fail("integrity_failed", "v2 ledger row field set is invalid", exit_code=4)
        if (led.get("schema_id") != "axissim.gemm-pair-study.ledger-row" or type(led.get("schema_version")) is not int or led["schema_version"] != 2 or
                led.get("study_row_id") != row["study_row_id"] or led.get("comparison_group_id") != row["comparison_group_id"] or
                led.get("base_case_id") != row["base_case_id"] or led.get("sensitivity_case_id") != row["sensitivity_case_id"] or
                not _is_exact_index(led.get("source_row_index"), index) or led.get("request_sha256") != row["request_sha256"]):
            _fail("integrity_failed", "v2 ledger identity/order differs from regenerated candidate", exit_code=4)
        status = led.get("runner_status")
        if status not in STATUS_VALUES or summ.get("runner_status") != status:
            _fail("integrity_failed", "v2 summary and ledger statuses disagree", exit_code=4)
        classified: dict[str, Any] = {
            "runner_status": status, "candidate_pair_id": led.get("candidate_pair_id"),
            "cpp_status": led.get("cpp_status"), "rvv_arm_status": led.get("rvv_arm_status"),
            "matrix_arm_status": led.get("matrix_arm_status"), "error_codes": led.get("error_codes", []),
            "error_paths": led.get("error_paths", []), "artifact_paths": led.get("artifact_paths", []),
        }
        if (not isinstance(led.get("error_codes"), list) or len(led["error_codes"]) > 4 or
                not isinstance(led.get("error_paths"), list) or len(led["error_paths"]) > 4 or
                not isinstance(led.get("artifact_paths"), list) or len(led["artifact_paths"]) > 2 or
                any(not isinstance(value, str) or not value.isascii() or len(value) > 64 for value in led["error_codes"]) or
                any(not isinstance(value, str) or not value.isascii() or len(value.encode("ascii")) > 256 for value in led["error_paths"]) or
                any(not isinstance(value, str) or not value.isascii() or len(value.encode("ascii")) > 128 for value in led["artifact_paths"])):
            _fail("integrity_failed", "v2 ledger error or artifact arrays exceed their contract", exit_code=4)
        _verify_terminal_row_fields(status, led, row)
        _verify_ledger_host_launch(row["study_row_id"], led, evaluator_by_row)
        if "execution_order" in led:
            orders.append(led["execution_order"])
            if (row["capacity_level_id"] == "p4" and led["execution_order"] >= 12) or (row["capacity_level_id"] == "p8" and led["execution_order"] < 12):
                _fail("integrity_failed", "v2 execution order violates the P4-pilot/P8 phase boundary", exit_code=4)
        for rel in led["artifact_paths"]:
            _safe_rel(rel)
            if not (root / rel).is_file():
                _fail("integrity_failed", "v2 ledger references a missing artifact", exit_code=4)
        for rel in led["artifact_paths"]:
            if rel.startswith("results/"):
                references.add(rel)
        if status in {"complete", "partial"}:
            rel = row["result_path"]
            raw = (root / rel).read_bytes()
            if not raw.endswith(b"\n") or len(raw) > row["result_payload_cap"] + 1:
                _fail("integrity_failed", "v2 C++ result violates payload cap or newline contract", exit_code=4)
            result = parse_json(raw[:-1], max_bytes=row["result_payload_cap"])
            valid, reason = validate_candidate_result(result, row, provenance["executable_sha256"])
            if not valid or result.get("status") != status or result.get("simulator") != provenance.get("cpp_simulator_identity"):
                _fail("integrity_failed", f"v2 C++ result failed contract validation: {reason}", exit_code=4)
            _verify_result_ledger_consistency(status, led, result, row)
            classified["record"] = result
            references.add(rel)
        elif status == "failed":
            if led.get("candidate_pair_id") == row["candidate_pair_id"] and led.get("artifact_paths") == [row["result_path"]]:
                raw = (root / row["result_path"]).read_bytes()
                if not raw.endswith(b"\n") or len(raw) > row["result_payload_cap"] + 1:
                    _fail("integrity_failed", "failed v2 result violates payload cap or newline contract", exit_code=4)
                result = parse_json(raw[:-1], max_bytes=row["result_payload_cap"])
                valid, reason = validate_candidate_result(result, row, provenance["executable_sha256"])
                if not valid or result.get("status") != "failed" or result.get("simulator") != provenance.get("cpp_simulator_identity"):
                    _fail("integrity_failed", f"failed v2 C++ result is malformed: {reason}", exit_code=4)
                _verify_result_ledger_consistency("failed", led, result, row)
                references.add(row["result_path"])
            else:
                error_path = led["artifact_paths"][-1]
                error, _ = read_json(root / error_path, max_bytes=V2_FILE_CAPS["manifest.json"])
                item = _verify_cpp_error_envelope(error, "internal_error", row)
                if (led.get("candidate_pair_id") is not None or led.get("cpp_status") != "internal_error" or
                        led.get("error_codes") != [item["code"]] or led.get("error_paths") != [item["path"]]):
                    _fail("integrity_failed", "v2 internal-error envelope differs from its ledger entry", exit_code=4)
                references.add(error_path)
        elif status == "invalid":
            expected_error = f"results/{row['study_row_id']}.error.json"
            expected_request = f"requests/{row['study_row_id']}.request.json"
            if led.get("artifact_paths") != [expected_request, expected_error] or led.get("candidate_pair_id") is not None:
                _fail("integrity_failed", "invalid v2 request must retain its row-scoped input and error envelope", exit_code=4)
            if (root / expected_request).read_bytes() != row["request_bytes"]:
                _fail("integrity_failed", "invalid v2 request artifact differs from canonical candidate input", exit_code=4)
            error, _ = read_json(root / expected_error, max_bytes=V2_FILE_CAPS["manifest.json"])
            if error.get("status") not in {"invalid_json", "input_limit", "unsupported_schema", "invalid_request"}:
                _fail("integrity_failed", "invalid v2 request has an unsupported C++ error status", exit_code=4)
            item = _verify_cpp_error_envelope(error, error["status"], row)
            if led.get("error_codes") != [item["code"]] or led.get("error_paths") != [item["path"]]:
                _fail("integrity_failed", "invalid v2 request envelope differs from its ledger entry", exit_code=4)
            references.add(expected_error)
        elif status in {"identity_mismatch", "result_schema_error"}:
            rel = led["artifact_paths"][-1]
            raw = (root / rel).read_bytes()
            try:
                malformed = parse_json(raw[:-1] if raw.endswith(b"\n") else raw,
                                       max_bytes=row["result_payload_cap"])
            except RunnerError:
                if status != "result_schema_error":
                    _fail("integrity_failed", "identity-mismatched v2 result is not valid JSON", exit_code=4)
            else:
                valid, reason = validate_candidate_result(malformed, row, provenance["executable_sha256"])
                if status == "identity_mismatch" and (valid or reason != "identity_mismatch"):
                    _fail("integrity_failed", "v2 identity-mismatch row lacks a result with identity mismatch", exit_code=4)
                if status == "result_schema_error" and (valid or reason != "result_schema_error"):
                    _fail("integrity_failed", "v2 schema-error row lacks a malformed result contract", exit_code=4)
            references.add(rel)
        elif status == "output_limit":
            if led.get("error_codes") == ["result_output_limit"]:
                if (led.get("candidate_pair_id") is not None or led.get("artifact_paths") or
                        led.get("error_paths") or any(field in led for field in ("cpp_status", "rvv_arm_status", "matrix_arm_status"))):
                    _fail("integrity_failed", "runner output cap cannot claim a C++ result or artifact", exit_code=4)
            else:
                if led.get("cpp_status") != "output_limit" or led.get("candidate_pair_id") != row["candidate_pair_id"] or led.get("artifact_paths") != [row["result_path"]]:
                    _fail("integrity_failed", "v2 C++ output-limit result has inconsistent ledger identity", exit_code=4)
                error, _ = read_json(root / row["result_path"], max_bytes=V2_FILE_CAPS["manifest.json"])
                item = _verify_cpp_error_envelope(error, "output_limit", row)
                if led.get("error_codes") != [item["code"]] or led.get("error_paths") != [item["path"]]:
                    _fail("integrity_failed", "v2 output-limit envelope differs from its ledger entry", exit_code=4)
                references.add(row["result_path"])
        if summ != _summary_row_v2(row, classified):
            _fail("integrity_failed", "v2 summary row differs from its candidate or ledger", exit_code=4)
        derived[index] = classified
    if sorted(orders) != list(range(len(orders))):
        _fail("integrity_failed", "v2 execution order is not contiguous", exit_code=4)
    if not _v2_launch_order_matches(rows, ledger):
        _fail("integrity_failed", "v2 evaluator launches do not follow exact planned P4/P8 row order", exit_code=4)
    if len([led for row, led in zip(rows, ledger) if row["capacity_level_id"] == "p4" and "execution_order" in led]) > 12:
        _fail("integrity_failed", "v2 pilot phase launched more than twelve P4 candidates", exit_code=4)
    if any(row["capacity_level_id"] == "p8" and "execution_order" in led for row, led in zip(rows, ledger)):
        p4 = [led for row, led in zip(rows, ledger) if row["capacity_level_id"] == "p4"]
        if preflight.get("status") != "ready" or len(p4) != 12 or any(item.get("runner_status") != "complete" for item in p4):
            _fail("integrity_failed", "P8 execution began without a fully passing twelve-case P4 gate", exit_code=4)
    if preflight.get("status") == "ready":
        p4 = [led for row, led in zip(rows, ledger) if row["capacity_level_id"] == "p4"]
        if len(p4) != 12 or any(item.get("runner_status") != "complete" for item in p4):
            _fail("integrity_failed", "ready pilot gate does not contain twelve complete P4 cases", exit_code=4)
    if preflight.get("status") == "pilot_failed":
        p4 = [led for row, led in zip(rows, ledger) if row["capacity_level_id"] == "p4"]
        if len(p4) != 12 or any("execution_order" not in item for item in p4) or any(row["capacity_level_id"] == "p8" and "execution_order" in led for row, led in zip(rows, ledger)):
            _fail("integrity_failed", "failed pilot gate must finish P4 pilots and must not launch P8", exit_code=4)
    _verify_diagnostic_ledger_set(ledger, evaluator_by_row)

    expected_hypotheses = _hypothesis_results_v2(manifest, rows, derived)
    if summary.get("hypotheses") != expected_hypotheses:
        _fail("integrity_failed", "v2 hypothesis outcomes differ from complete C++ result records", exit_code=4)
    if preflight["status"] != "ready":
        if summary.get("study_status") != preflight["status"]:
            _fail("integrity_failed", "terminal preflight and summary status disagree", exit_code=4)
    elif summary.get("study_status") not in {"complete", "incomplete", "limit_reached"}:
        _fail("integrity_failed", "v2 execution summary has an unsupported terminal status", exit_code=4)

    actual_results = {path for path in paths if path.startswith("results/") and path.endswith((".result.json", ".error.json"))}
    if actual_results != references:
        _fail("integrity_failed", "v2 result artifacts and ledger references differ", exit_code=4)
    actual_requests = {path for path in paths if path.startswith("requests/") and path.endswith(".request.json")}
    allowed_identityless: set[str] = set()
    for row, led in zip(rows, ledger):
        for rel in led["artifact_paths"]:
            if rel.startswith("requests/") and rel not in request_paths:
                expected = f"requests/{row['study_row_id']}.request.json"
                if rel != expected or (root / rel).read_bytes() != row["request_bytes"]:
                    _fail("integrity_failed", "v2 identityless request differs from planned canonical request", exit_code=4)
                allowed_identityless.add(rel)
    if actual_requests != request_paths | allowed_identityless:
        _fail("integrity_failed", "v2 request artifact set differs from the plan", exit_code=4)
    if paths != required | request_paths | allowed_identityless | actual_results:
        _fail("integrity_failed", "v2 bundle contains unexpected contract paths", exit_code=4)
    if summary["study_status"] == "complete" and any(item["runner_status"] != "complete" for item in ledger):
        _fail("integrity_failed", "v2 study marked complete with incomplete candidate rows", exit_code=4)
    if (root / "summary.csv").read_bytes() != _summary_csv(rows, derived, summary["rows"]).encode("ascii"):
        _fail("integrity_failed", "v2 CSV differs from validated C++ results", exit_code=4)
    if (root / "report.md").read_bytes() != _make_report_v2(manifest, rows, derived, summary["study_status"]).encode("utf-8"):
        _fail("integrity_failed", "v2 report differs from validated outcomes", exit_code=4)
    for rel, data in _make_v2_plots(rows, derived).items():
        if (root / rel).read_bytes() != data:
            _fail("integrity_failed", f"v2 plot differs from validated C++ results: {rel}", exit_code=4)
    diag, _ = read_jsonl(root / "host-diagnostics.jsonl")
    elapsed = sum(item.get("phase_elapsed_ns", 0) for item in diag if item.get("record_type") == "phase")
    if (elapsed > V2_LIMITS["max_active_seconds"] * 1_000_000_000 and
            preflight.get("status") != "limit_reached" and summary.get("study_status") != "limit_reached"):
        _fail("integrity_failed", "v2 recorded active time exceeds 900 seconds without a limit_reached status", exit_code=4)
    if any(item.get("evaluator_elapsed_ns", 0) > V2_LIMITS["max_evaluator_seconds"] * 1_000_000_000 for item in diag if item.get("record_type") == "evaluator"):
        _fail("integrity_failed", "v2 evaluator diagnostic exceeds the 30-second invocation limit", exit_code=4)


def _verify_bundle_contract(root: Path, summary: dict[str, Any], ledger: list[dict[str, Any]],
                            evaluator_by_row: dict[str, dict[str, Any]]) -> None:
    manifest_obj, _ = read_json(root / "manifest.json", max_bytes=FILE_CAPS["manifest.json"])
    if isinstance(manifest_obj, dict) and manifest_obj.get("schema_version") == 2:
        _verify_bundle_contract_v2(root, summary, ledger, evaluator_by_row)
        return
    required_files = {
        "manifest.json", "plan.json", "preflight.json", "provenance.json", "runner-ledger.jsonl",
        "summary.json", "summary.csv", "report.md", "plots/compute-cycles.svg", "plots/logical-payload-bytes.svg",
        "canonical-artifact-manifest.json", "host-diagnostics.jsonl", "bundle-index.json",
    }
    paths = _actual_file_paths(root)
    if not required_files.issubset(paths):
        _fail("integrity_failed", "bundle is missing required phase/report files", exit_code=4)
    manifest_obj, manifest_raw = read_json(root / "manifest.json", max_bytes=FILE_CAPS["manifest.json"])
    manifest = validate_manifest(manifest_obj)
    if manifest_raw != canonical_bytes(manifest) + b"\n":
        _fail("integrity_failed", "manifest artifact is not canonical JSON plus one LF", exit_code=4)
    manifest_id = canonical_hash(manifest)
    generated = expand_candidates(manifest, manifest_id)
    plan, plan_raw = read_json(root / "plan.json", max_bytes=FILE_CAPS["plan.json"])
    _verify_plan_record(plan, plan_raw, make_plan(manifest, manifest_id, generated))
    for row in generated:
        request_path = root / row["request_path"]
        if not request_path.is_file() or request_path.read_bytes() != row["request_bytes"]:
            _fail("integrity_failed", "planned canonical request is missing or changed", exit_code=4)
    expected_request_paths = {row["request_path"] for row in generated if row["duplicate_of"] is None}
    preflight, _ = read_json(root / "preflight.json", max_bytes=FILE_CAPS["preflight.json"])
    expected_pf_keys = {
        "schema_id", "schema_version", "manifest_id", "plan_sha256", "study_run_id", "executable_sha256",
        "status", "source_row_count", "unique_candidate_count", "exact_request_file_bytes", "reserved_peak_bytes",
        "worst_case_evaluator_seconds", "pilot_row_ids", "error_codes",
    }
    pf_statuses = {"ready", "pilot_failed", "limit_reached", "provenance_failed", "invalid_manifest", "output_limit"}
    if not _has_schema_v1(preflight, "axissim.gemm-pair-study.preflight") or set(preflight) != expected_pf_keys or preflight.get("status") not in pf_statuses:
        _fail("integrity_failed", "preflight schema or status is invalid", exit_code=4)
    request_bytes, _, peak_bytes = _compute_storage_bound(root, generated, len(generated))
    pilots = _pilot_candidates(generated, manifest)
    expected_pf_values = {
        "manifest_id": manifest_id,
        "plan_sha256": sha256_bytes(canonical_bytes(plan)),
        "source_row_count": len(generated),
        "unique_candidate_count": len({row["request_bytes"] for row in generated}),
        "exact_request_file_bytes": request_bytes,
        "reserved_peak_bytes": peak_bytes,
        "worst_case_evaluator_seconds": len({row["request_bytes"] for row in generated}) * FIXED_LIMITS["max_evaluator_seconds"],
        "pilot_row_ids": [row["study_row_id"] for row in pilots],
    }
    if any(preflight.get(key) != value for key, value in expected_pf_values.items()):
        _fail("integrity_failed", "preflight values do not match manifest and plan", exit_code=4)
    provenance, _ = read_json(root / "provenance.json", max_bytes=FILE_CAPS["provenance.json"])
    provenance_keys = {
        "schema_id", "schema_version", "manifest_id", "study_label", "study_run_id", "manifest_input_sha256",
        "runner_source_sha256", "source_checkout_id", "source_revision", "source_tree_state", "dirty_diff_sha256",
        "executable_sha256", "cpp_simulator_identity", "request_sha256s",
    }
    if not _has_schema_v1(provenance, "axissim.gemm-pair-study.provenance") or set(provenance) != provenance_keys:
        _fail("integrity_failed", "provenance schema is invalid", exit_code=4)
    if provenance.get("manifest_id") != manifest_id or provenance.get("study_label") != manifest["study_label"] or provenance.get("study_run_id") != preflight.get("study_run_id") or provenance.get("executable_sha256") != preflight.get("executable_sha256") or provenance.get("request_sha256s") != [r["request_sha256"] for r in generated]:
        _fail("integrity_failed", "provenance identities do not match preflight", exit_code=4)
    for key in ("manifest_input_sha256", "runner_source_sha256", "source_checkout_id"):
        if not _is_sha256(provenance.get(key)) and not (preflight.get("status") == "provenance_failed" and provenance.get(key) == "unknown"):
            _fail("integrity_failed", f"invalid provenance hash field {key}", exit_code=4)
    if preflight.get("status") == "ready":
        if not _is_sha256(preflight.get("study_run_id")) or not _is_sha256(preflight.get("executable_sha256")) or preflight.get("error_codes") != []:
            _fail("integrity_failed", "ready preflight lacks complete identities", exit_code=4)
    elif preflight.get("status") == "provenance_failed":
        if preflight.get("study_run_id") != "unknown" or preflight.get("executable_sha256") != "unknown":
            _fail("integrity_failed", "provenance-failed preflight must not claim identities", exit_code=4)
    elif not isinstance(preflight.get("error_codes"), list) or not preflight["error_codes"]:
        _fail("integrity_failed", "failed preflight needs a stable error code", exit_code=4)
    if not _has_schema_v1(summary, "axissim.gemm-pair-study.summary") or set(summary) != {"schema_id", "schema_version", "study_label", "study_run_id", "study_status", "counts", "rows"}:
        _fail("integrity_failed", "summary schema is invalid", exit_code=4)
    if summary.get("study_label") != manifest["study_label"] or summary.get("study_run_id") != preflight.get("study_run_id"):
        _fail("integrity_failed", "summary identity differs from provenance", exit_code=4)
    if summary.get("study_status") not in {"complete", "incomplete", "limit_reached", "invalid_manifest", "pilot_failed", "provenance_failed", "output_limit", "integrity_failed"}:
        _fail("integrity_failed", "summary has an unsupported study status", exit_code=4)
    expected_count_keys = set(STATUS_VALUES)
    counts = summary.get("counts")
    if not isinstance(counts, dict) or set(counts) != expected_count_keys or any(isinstance(v, bool) or not isinstance(v, int) or v < 0 for v in counts.values()):
        _fail("integrity_failed", "summary status counts are malformed", exit_code=4)
    if not isinstance(summary.get("rows"), list) or len(summary["rows"]) != len(generated) or len(ledger) != len(generated):
        _fail("integrity_failed", "summary and ledger must contain one row per planned row", exit_code=4)
    if sum(counts.values()) != len(generated):
        _fail("integrity_failed", "summary status counts do not total planned rows", exit_code=4)
    for status, count in counts.items():
        if count != sum(1 for item in summary["rows"] if item.get("runner_status") == status):
            _fail("integrity_failed", "summary status counts disagree with summary rows", exit_code=4)
    execution_orders: list[int] = []
    referenced_results: set[str] = set()
    for index, (row, led, summ) in enumerate(zip(generated, ledger, summary["rows"])):
        required_ledger = {"schema_id", "schema_version", "study_row_id", "comparison_group_id", "source_row_index", "runner_status", "error_codes", "error_paths", "artifact_paths"}
        optional_ledger = {"execution_order", "process_exit_code", "request_sha256", "candidate_pair_id", "cpp_status", "rvv_arm_status", "matrix_arm_status"}
        if not isinstance(led, dict) or not required_ledger.issubset(led) or not set(led).issubset(required_ledger | optional_ledger):
            _fail("integrity_failed", "ledger row has an invalid field set", exit_code=4)
        if not _has_schema_v1(led, "axissim.gemm-pair-study.ledger-row") or led.get("study_row_id") != row["study_row_id"] or led.get("comparison_group_id") != row["comparison_group_id"] or not _is_exact_index(led.get("source_row_index"), index) or led.get("request_sha256") != row["request_sha256"]:
            _fail("integrity_failed", "ledger identity or ordering differs from plan", exit_code=4)
        status = led.get("runner_status")
        if status not in STATUS_VALUES or summ.get("study_row_id") != row["study_row_id"] or not _is_exact_index(summ.get("source_row_index"), index) or summ.get("comparison_group_id") != row["comparison_group_id"] or summ.get("runner_status") != status:
            _fail("integrity_failed", "summary row and ledger identity/status disagree", exit_code=4)
        for field in ("candidate_pair_id", "cpp_status", "rvv_arm_status", "matrix_arm_status"):
            if summ.get(field) != led.get(field):
                _fail("integrity_failed", f"summary and ledger {field} differ", exit_code=4)
        if summ.get("error_codes", []) != led.get("error_codes", []) or summ.get("artifact_paths", []) != led.get("artifact_paths", []):
            _fail("integrity_failed", "summary error/artifact references differ from ledger", exit_code=4)
        if summ.get("shape_id") != row["shape_id"] or summ.get("capacity_level_id") != row["capacity_level_id"]:
            _fail("integrity_failed", "summary row candidate assignment differs from plan", exit_code=4)
        if summ.get("workload_dimensions") != {k: row["request"]["workload"][k] for k in ("m", "n", "k")}:
            _fail("integrity_failed", "summary workload dimensions differ from request", exit_code=4)
        if summ.get("mapping_parameters") != row["request"]["mappings"] or summ.get("capacity_assumption") != {
            "mac_slots_per_cycle": row["request"]["mappings"]["matrix"]["mac_slots_per_cycle"],
            "physical_cost_equivalence": "not_claimed",
        }:
            _fail("integrity_failed", "summary mapping assumptions differ from canonical request", exit_code=4)
        if not _is_exact_index(summ.get("source_row_index"), index):
            _fail("integrity_failed", "summary source row index is invalid", exit_code=4)
        if status != "complete" and "metrics" in summ:
            _fail("integrity_failed", "ineligible row must not contain paired metrics", exit_code=4)
        if "execution_order" in led:
            value = led["execution_order"]
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                _fail("integrity_failed", "ledger execution order is invalid", exit_code=4)
            execution_orders.append(value)
        _verify_ledger_host_launch(row["study_row_id"], led, evaluator_by_row)
        if not isinstance(led.get("error_codes"), list) or len(led["error_codes"]) > 4 or any(not isinstance(x, str) or not x.isascii() or len(x) > 64 for x in led["error_codes"]):
            _fail("integrity_failed", "ledger error codes are malformed", exit_code=4)
        if not isinstance(led.get("error_paths"), list) or len(led["error_paths"]) > 4 or any(not isinstance(x, str) or not x.isascii() or len(x.encode("ascii")) > 256 for x in led["error_paths"]):
            _fail("integrity_failed", "ledger error paths are malformed", exit_code=4)
        if not isinstance(led.get("artifact_paths"), list) or len(led["artifact_paths"]) > 2:
            _fail("integrity_failed", "ledger artifact paths are malformed", exit_code=4)
        for rel in led["artifact_paths"]:
            if not isinstance(rel, str) or len(rel.encode("ascii", errors="ignore")) > 128:
                _fail("integrity_failed", "ledger artifact path exceeds its ASCII path limit", exit_code=4)
            _safe_rel(rel)
        _verify_terminal_row_fields(status, led, row)
        if row["duplicate_of"] is not None:
            if status != "duplicate" or "execution_order" in led or led["artifact_paths"] or led.get("candidate_pair_id") != row["candidate_pair_id"]:
                _fail("integrity_failed", "duplicate row must reference the first candidate without artifacts", exit_code=4)
        elif status == "complete":
            if led.get("candidate_pair_id") != row["candidate_pair_id"] or led.get("cpp_status") != "complete" or led.get("process_exit_code") != 0 or led.get("artifact_paths") != [row["result_path"]] or "metrics" not in summ:
                _fail("integrity_failed", "complete row is missing a valid paired result", exit_code=4)
            raw_result = (root / row["result_path"]).read_bytes()
            result = parse_json(raw_result[:-1])
            valid, reason = validate_candidate_result(result, row, provenance["executable_sha256"])
            if not valid or result.get("status") != "complete" or result.get("simulator") != provenance.get("cpp_simulator_identity"):
                _fail("integrity_failed", f"complete result failed contract validation: {reason}", exit_code=4)
            referenced_results.add(row["result_path"])
            if summ["metrics"] != _metrics_for(result):
                _fail("integrity_failed", "summary metrics differ from authoritative C++ result", exit_code=4)
        elif status == "partial":
            if led.get("candidate_pair_id") != row["candidate_pair_id"] or led.get("cpp_status") != "partial" or led.get("process_exit_code") != 3 or led.get("artifact_paths") != [row["result_path"]]:
                _fail("integrity_failed", "partial row is missing its structured C++ result", exit_code=4)
            result_raw = (root / row["result_path"]).read_bytes()
            result = parse_json(result_raw[:-1])
            valid, reason = validate_candidate_result(result, row, provenance["executable_sha256"])
            if not valid or result.get("status") != "partial" or result.get("simulator") != provenance.get("cpp_simulator_identity"):
                _fail("integrity_failed", f"partial result failed contract validation: {reason}", exit_code=4)
            referenced_results.add(row["result_path"])
        elif status == "failed":
            if led.get("process_exit_code") != 4:
                _fail("integrity_failed", "failed evaluator row must have process exit 4", exit_code=4)
            if led.get("candidate_pair_id") == row["candidate_pair_id"] and led.get("artifact_paths") == [row["result_path"]]:
                result_raw = (root / row["result_path"]).read_bytes()
                result = parse_json(result_raw[:-1])
                valid, reason = validate_candidate_result(result, row, provenance["executable_sha256"])
                if not valid or result.get("status") != "failed" or result.get("simulator") != provenance.get("cpp_simulator_identity"):
                    _fail("integrity_failed", f"failed C++ result is malformed: {reason}", exit_code=4)
                referenced_results.add(row["result_path"])
            elif led.get("candidate_pair_id") is None and led.get("artifact_paths") in (
                [f"results/{row['study_row_id']}.error.json"],
                [f"requests/{row['study_row_id']}.request.json", f"results/{row['study_row_id']}.error.json"],
            ):
                error_path = led["artifact_paths"][-1]
                error, _ = read_json(root / error_path, max_bytes=FILE_CAPS["manifest.json"])
                item = _verify_cpp_error_envelope(error, "internal_error", row)
                if led.get("error_codes") != [item["code"]] or led.get("error_paths") != [item["path"]]:
                    _fail("integrity_failed", "internal-error envelope differs from its ledger entry", exit_code=4)
                referenced_results.add(error_path)
            else:
                _fail("integrity_failed", "failed row is missing a structured C++ error/result", exit_code=4)
        elif status == "invalid":
            expected_error = f"results/{row['study_row_id']}.error.json"
            expected_request = f"requests/{row['study_row_id']}.request.json"
            if led.get("candidate_pair_id") is not None or led.get("process_exit_code") != 2 or led.get("artifact_paths") != [expected_request, expected_error]:
                _fail("integrity_failed", "invalid request must retain the study-row request and error envelope", exit_code=4)
            error, _ = read_json(root / expected_error, max_bytes=FILE_CAPS["manifest.json"])
            if not isinstance(error, dict) or error.get("status") not in {"invalid_json", "input_limit", "unsupported_schema", "invalid_request"}:
                _fail("integrity_failed", "invalid request error envelope has an unsupported status", exit_code=4)
            _verify_cpp_error_envelope(error, error["status"], row)
            if led.get("error_codes") != _error_codes(error) or led.get("error_paths") != _error_paths(error):
                _fail("integrity_failed", "invalid-request envelope differs from its ledger entry", exit_code=4)
            referenced_results.add(expected_error)
        elif status in {"identity_mismatch", "result_schema_error"}:
            rel = led["artifact_paths"][-1]
            raw = (root / rel).read_bytes()
            try:
                malformed_record = parse_json(raw[:-1] if raw.endswith(b"\n") else raw)
            except RunnerError:
                if status != "result_schema_error":
                    _fail("integrity_failed", "identity-mismatched result is not valid JSON", exit_code=4)
            else:
                valid, reason = validate_candidate_result(malformed_record, row, provenance["executable_sha256"])
                if status == "identity_mismatch" and (valid or reason != "identity_mismatch"):
                    _fail("integrity_failed", "identity-mismatch row does not contain a result with an identity mismatch", exit_code=4)
                if status == "result_schema_error" and (valid or reason != "result_schema_error"):
                    _fail("integrity_failed", "schema-error row does not contain a malformed result contract", exit_code=4)
            referenced_results.add(rel)
        elif status == "output_limit":
            if "result_output_limit" in led["error_codes"]:
                if led.get("error_codes") != ["result_output_limit"] or led.get("candidate_pair_id") is not None or any(field in led for field in ("cpp_status", "rvv_arm_status", "matrix_arm_status")) or led.get("artifact_paths") or led.get("error_paths") != []:
                    _fail("integrity_failed", "runner result-size limit cannot claim candidate or C++ result fields", exit_code=4)
            else:
                if led.get("cpp_status") != "output_limit" or led.get("candidate_pair_id") != row["candidate_pair_id"] or led.get("artifact_paths") != [row["result_path"]]:
                    _fail("integrity_failed", "C++ output-limit result has inconsistent ledger identity", exit_code=4)
                error, _ = read_json(root / row["result_path"], max_bytes=FILE_CAPS["manifest.json"])
                item = _verify_cpp_error_envelope(error, "output_limit", row)
                if led.get("error_codes") != [item["code"]] or led.get("error_paths") != [item["path"]]:
                    _fail("integrity_failed", "output-limit envelope differs from its ledger entry", exit_code=4)
                referenced_results.add(row["result_path"])
        else:
            if "metrics" in summ:
                _fail("integrity_failed", "ineligible row must not contain paired metrics", exit_code=4)
            for rel in led["artifact_paths"]:
                _safe_rel(rel)
                if not (root / rel).is_file():
                    _fail("integrity_failed", "ledger references a missing artifact", exit_code=4)
                if rel.startswith("results/"):
                    referenced_results.add(rel)
    if sorted(execution_orders) != list(range(len(execution_orders))):
        _fail("integrity_failed", "execution order is not a contiguous deterministic sequence", exit_code=4)
    _verify_diagnostic_ledger_set(ledger, evaluator_by_row)
    actual_results = {p for p in paths if p.startswith("results/") and p.endswith((".result.json", ".error.json"))}
    if actual_results != referenced_results:
        _fail("integrity_failed", "result artifacts and ledger references differ", exit_code=4)
    allowed_identityless_requests: set[str] = set()
    for row, led in zip(generated, ledger):
        for rel in led["artifact_paths"]:
            if rel.startswith("requests/") and rel not in expected_request_paths:
                expected_rel = f"requests/{row['study_row_id']}.request.json"
                if rel != expected_rel or (root / rel).read_bytes() != row["request_bytes"]:
                    _fail("integrity_failed", "identityless request artifact does not match its study row", exit_code=4)
                allowed_identityless_requests.add(rel)
    actual_request_paths = {p for p in paths if p.startswith("requests/") and p.endswith(".request.json")}
    if actual_request_paths != expected_request_paths | allowed_identityless_requests:
        _fail("integrity_failed", "request artifacts include missing or unexpected paths", exit_code=4)
    allowed_files = required_files | expected_request_paths | allowed_identityless_requests | actual_results
    if paths != allowed_files:
        _fail("integrity_failed", f"bundle contains unexpected contract paths: {sorted(paths-allowed_files)}", exit_code=4)
    if summary["study_status"] == "complete" and any(
        led["runner_status"] not in {"complete", "duplicate"} for led in ledger
    ):
        _fail("integrity_failed", "study marked complete despite incomplete rows", exit_code=4)


def verify_bundle(root: Path) -> dict[str, Any]:
    root = Path(root).expanduser()
    if root.is_symlink():
        _fail("unsafe_path", "bundle root cannot be a symlink", exit_code=4)
    root = root.resolve(strict=True)
    manifest_path = _check_no_symlink(root, "manifest.json")
    manifest_obj, _ = read_json(manifest_path, max_bytes=V2_FILE_CAPS["manifest.json"])
    study_manifest = validate_manifest(manifest_obj)
    study_schema_version = study_manifest["schema_version"]
    caps = _caps_for_manifest(study_manifest)
    artifact_manifest, _ = read_json(root / "canonical-artifact-manifest.json", max_bytes=FILE_CAPS["canonical-artifact-manifest.json"])
    if not _has_schema_v1(artifact_manifest, "axissim.gemm-pair-study.artifacts"):
        _fail("integrity_failed", "unsupported artifact manifest", exit_code=4)
    _check_path_set(root, artifact_manifest, study_schema_version)
    index, index_raw = read_json(root / "bundle-index.json", max_bytes=FILE_CAPS["bundle-index.json"])
    if not _has_schema_v1(index, "axissim.gemm-pair-study.bundle-index") or set(index) != {"schema_id", "schema_version", "canonical_artifact_manifest_sha256", "host_diagnostics_sha256"}:
        _fail("integrity_failed", "unsupported bundle index", exit_code=4)
    if index.get("canonical_artifact_manifest_sha256") != sha256_bytes((root / "canonical-artifact-manifest.json").read_bytes()):
        _fail("integrity_failed", "canonical artifact manifest hash mismatch", exit_code=4)
    if index.get("host_diagnostics_sha256") != sha256_bytes((root / "host-diagnostics.jsonl").read_bytes()):
        _fail("integrity_failed", "host diagnostics hash mismatch", exit_code=4)
    diag, _ = read_jsonl(root / "host-diagnostics.jsonl")
    evaluator_by_row = _verify_host_diagnostics(diag)
    ledger, _ = read_jsonl(root / "runner-ledger.jsonl")
    summary, _ = read_json(root / "summary.json", max_bytes=caps["summary.json"])
    if (not isinstance(summary, dict) or summary.get("schema_id") != "axissim.gemm-pair-study.summary" or
            type(summary.get("schema_version")) is not int or summary.get("schema_version") != study_schema_version):
        _fail("integrity_failed", "unsupported summary schema", exit_code=4)
    seen_rows = set()
    for rec in ledger:
        if (not isinstance(rec, dict) or rec.get("schema_id") != "axissim.gemm-pair-study.ledger-row" or
                type(rec.get("schema_version")) is not int or rec.get("schema_version") != study_schema_version):
            _fail("integrity_failed", "unsupported ledger row schema", exit_code=4)
        row_id = rec.get("study_row_id")
        if row_id in seen_rows or rec.get("runner_status") not in STATUS_VALUES:
            _fail("integrity_failed", "duplicate row or invalid terminal status", exit_code=4)
        seen_rows.add(row_id)
    if len(summary.get("rows", [])) != len(ledger):
        _fail("integrity_failed", "summary row count differs from ledger", exit_code=4)
    for led, summ in zip(ledger, summary["rows"]):
        if led["study_row_id"] != summ.get("study_row_id") or led["runner_status"] != summ.get("runner_status"):
            _fail("integrity_failed", "summary and ledger order/status differ", exit_code=4)
    _verify_bundle_contract(root, summary, ledger, evaluator_by_row)
    return {"status": "verified", "artifact_count": len(artifact_manifest["artifacts"]), "ledger_rows": len(ledger), "diagnostic_records": len(diag), "bundle_bytes": file_bytes_under(root)}


def command_verify(args: argparse.Namespace) -> int:
    result = verify_bundle(Path(args.bundle).expanduser())
    print(f"Bundle verified: {result['artifact_count']} artifacts, {result['ledger_rows']} rows, {result['bundle_bytes']} bytes")
    return 0


def _compare_replay_artifacts(reference: Path, replay: Path) -> bool:
    ref_manifest, _ = read_json(reference / "canonical-artifact-manifest.json")
    new_manifest, _ = read_json(replay / "canonical-artifact-manifest.json")
    if ref_manifest != new_manifest:
        print("Replay artifact path/hash manifests differ", file=sys.stderr)
        return False
    for entry in ref_manifest["artifacts"]:
        rel = entry["path"]
        if (reference / rel).read_bytes() != (replay / rel).read_bytes():
            print(f"Replay differs at {rel}", file=sys.stderr)
            return False
    return True


def command_reproduce(args: argparse.Namespace) -> int:
    # Use the same public phase functions and a fresh destination; never overwrite a prior run.
    ref_arg = Path(args.reference_bundle).expanduser()
    if ref_arg.is_symlink():
        _fail("unsafe_path", "reference bundle root must not be a symlink", exit_code=4)
    ref = ref_arg.resolve(strict=True)
    verify_bundle(ref)
    output_path = Path(args.output_dir).expanduser()
    if output_path.exists() or output_path.is_symlink():
        _fail("output_exists", "reproduction output directory must not exist", exit_code=4)
    plan_args = argparse.Namespace(manifest=args.manifest, output_dir=str(output_path))
    root = output_path.resolve(strict=False)
    command_plan(plan_args)
    preflight_args = argparse.Namespace(manifest=args.manifest, binary=args.binary, output_dir=str(root))
    preflight_exit = command_preflight(preflight_args)
    if preflight_exit != 0:
        # A deterministic pilot/limit/provenance failure is still a valid
        # terminal bundle. Verify and compare it rather than treating the
        # phase's nonzero status as an uninspectable reproduction failure.
        try:
            verify_bundle(root)
            replay_preflight, _ = read_json(root / "preflight.json", max_bytes=FILE_CAPS["preflight.json"])
        except (RunnerError, OSError):
            return 4
        if replay_preflight.get("status") == "ready":
            return 4
        if not _compare_replay_artifacts(ref, root):
            return 6
        print(f"Replay reproduced terminal preflight status {replay_preflight['status']}; canonical artifacts match")
        return 0
    if command_execute(preflight_args) not in (0, 3):
        return 4
    verify_bundle(root)
    if not _compare_replay_artifacts(ref, root):
        return 6
    print("Replay canonical artifacts match byte-for-byte; host diagnostics were integrity-checked separately")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Bounded paired-GEMM study runner")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "preflight", "execute"):
        cmd = sub.add_parser(name)
        cmd.add_argument("--manifest", required=True)
        cmd.add_argument("--output-dir", required=True)
        if name in ("preflight", "execute"):
            cmd.add_argument("--binary", required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("--bundle", required=True)
    reproduce = sub.add_parser("reproduce")
    reproduce.add_argument("--manifest", required=True)
    reproduce.add_argument("--binary", required=True)
    reproduce.add_argument("--reference-bundle", required=True)
    reproduce.add_argument("--output-dir", required=True)
    args = parser.parse_args(argv)
    try:
        return {
            "plan": command_plan,
            "preflight": command_preflight,
            "execute": command_execute,
            "verify": command_verify,
            "reproduce": command_reproduce,
        }[args.command](args)
    except RunnerError as exc:
        print(f"{exc.code}: {exc}", file=sys.stderr)
        return exc.exit_code
    except KeyboardInterrupt:
        print("interrupted; output root preserved for diagnosis", file=sys.stderr)
        return 3
    except Exception as exc:
        print(f"internal_error: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
