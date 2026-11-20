"""Strict parser and verifier for AxisSim's run-provenance sidecar contract."""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any


class RawNumber(str):
    """JSON number preserving the exact lexical form emitted by C++."""


_CONFIG_STRING_FIELDS = frozenset({
    "scenario_name", "model_version", "workload_mode", "workload_profile",
    "mapping_plan", "scheduler_policy", "scheduler_ordering_policy",
    "frame_arrival_mode", "frame_arrival_trace", "frame_arrival_provenance",
    "rvv_timing_model", "rvv_memory_accounting",
})
_CONFIG_SIGNED_INTEGER_FIELDS = frozenset({
    "frames", "rvv_vlen_bits", "rvv_sew_bits", "rvv_lmul", "rvv_lanes",
    "rvv_ops_per_lane_cycle", "npu_macs", "preprocess_ops_per_element",
    "gemm_m", "gemm_n", "gemm_k", "npu_tile_m", "npu_tile_n",
    "npu_tile_k", "npu_element_bytes", "npu_accumulator_bytes",
    "npu_weights_resident", "dma_double_buffering", "rvv_memory_bytes_per_element",
    "postprocess_ops_per_element",
    "deployment_xpu_count",
})
_CONFIG_UNSIGNED_INTEGER_FIELDS = frozenset({
    "control_ops", "rvv_chunk_elements", "input_bytes", "preprocess_elements",
    "preprocess_output_bytes", "sram_capacity_bytes", "weight_sram_capacity_bytes",
    "postprocess_elements", "postprocess_bytes",
})
_CONFIG_FLOAT_FIELDS = frozenset({
    "frame_period_us", "deadline_us", "dram_bandwidth_gbps", "dma_setup_us",
    "control_period_us", "control_deadline_us", "rvv_frequency_ghz",
    "rvv_stripmine_overhead_cycles", "rvv_affine_overhead_cycles",
    "rvv_compute_cycle_scale", "npu_frequency_ghz", "vector_op_pj", "npu_mac_pj",
    "npu_arithmetic_energy_scale", "arithmetic_voltage_ratio", "dram_byte_pj",
    "dma_byte_pj", "static_power_w", "npu_static_power_per_mac_w",
    "rvv_static_power_per_lane_w", "sram_static_power_per_kib_w",
    "dram_interface_static_power_w", "it_power_budget_w", "pue",
    "electricity_cost_per_kwh", "deployment_hours", "base_area_mm2",
    "npu_area_per_mac_mm2", "rvv_area_per_lane_mm2", "sram_area_per_kib_mm2",
    "max_area_mm2", "ambient_temp_c", "thermal_resistance_c_per_w",
    "thermal_capacitance_j_per_c", "max_junction_temp_c", "reliability_fit_rate",
    "reliability_voltage_fit_multiplier",
})
_REQUIRED_CONFIG_FIELDS = (
    _CONFIG_STRING_FIELDS | _CONFIG_SIGNED_INTEGER_FIELDS |
    _CONFIG_UNSIGNED_INTEGER_FIELDS | _CONFIG_FLOAT_FIELDS
)


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-standard JSON numeric value is not allowed: {value}")


def _object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key is not allowed: {key}")
        result[key] = value
    return result


def _quote(value: str) -> str:
    pieces = ['"']
    for character in value:
        codepoint = ord(character)
        if character in ('"', "\\"):
            pieces.extend(("\\", character))
        elif codepoint < 0x20:
            pieces.append(f"\\u00{codepoint:02x}")
        else:
            pieces.append(character)
    pieces.append('"')
    return "".join(pieces)


def canonical_json(value: Any) -> str:
    """Match the C++ sorted-key, no-whitespace identity serialization."""
    if isinstance(value, RawNumber):
        return str(value)
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, list):
        return "[" + ",".join(canonical_json(child) for child in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(
            _quote(key) + ":" + canonical_json(value[key]) for key in sorted(value)
        ) + "}"
    raise TypeError(f"unsupported provenance value: {type(value).__name__}")


def _valid_sha256(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _known_identity_string(value: Any) -> bool:
    return isinstance(value, str) and value.strip() not in ("", "unknown")


def _complete_identity_issue(payload: dict[str, Any], schema: str) -> str | None:
    """Check that a complete claim has the identity evidence it asserts."""
    source = payload.get("source")
    if not isinstance(source, dict):
        return "complete run provenance has no source identity"
    revision = source.get("revision")
    if not isinstance(revision, str) or re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", revision) is None:
        return "complete run provenance has no valid source revision"
    if source.get("tree_state") != "clean":
        return "complete run provenance does not identify a clean source tree"

    build = payload.get("build")
    if not isinstance(build, dict) or any(
        not _known_identity_string(build.get(field))
        for field in ("compiler_id", "compiler_version", "cxx_standard", "build_type", "platform", "cmake_version")
    ):
        return "complete run provenance has incomplete build/toolchain identity"
    executable = payload.get("executable")
    if (not isinstance(executable, dict) or executable.get("status") != "hashed" or
            not _valid_sha256(executable.get("sha256"))):
        return "complete run provenance has no verified executable hash"

    manifest = payload.get("manifest")
    manifest_identity = manifest.get("identity") if isinstance(manifest, dict) else None
    if (not isinstance(manifest, dict) or not _known_identity_string(manifest.get("path")) or
            not isinstance(manifest_identity, dict) or
            manifest_identity.get("role") != "root_configuration" or
            manifest_identity.get("status") != "hashed" or
            not _valid_sha256(manifest_identity.get("sha256"))):
        return "complete run provenance has no hashed root configuration manifest"

    files = payload.get("configuration_files")
    if not isinstance(files, list) or not files:
        return "complete run provenance has no configuration file inventory"
    root_records = []
    for item in files:
        if (not isinstance(item, dict) or not _known_identity_string(item.get("role")) or
                not _known_identity_string(item.get("path")) or item.get("status") != "hashed" or
                not _valid_sha256(item.get("sha256"))):
            return "complete run provenance has an unavailable or malformed input hash"
        if item.get("role") == "root_configuration":
            root_records.append(item)
    if (len(root_records) != 1 or
            root_records[0].get("path") != manifest_identity.get("path") or
            root_records[0].get("sha256") != manifest_identity.get("sha256")):
        return "complete run provenance root manifest does not match its input inventory"

    config = payload.get("configuration")
    if not isinstance(config, dict) or not _REQUIRED_CONFIG_FIELDS.issubset(config):
        return "complete run provenance lacks the resolved configuration fields"
    if any(not isinstance(config.get(field), str) for field in _CONFIG_STRING_FIELDS):
        return "complete run provenance has invalid resolved configuration string fields"
    if any(not isinstance(config.get(field), RawNumber) or
           re.fullmatch(r"-?(?:0|[1-9][0-9]*)", config[field]) is None
           for field in _CONFIG_SIGNED_INTEGER_FIELDS):
        return "complete run provenance has invalid resolved configuration integer fields"
    if any(not isinstance(config.get(field), RawNumber) or
           re.fullmatch(r"(?:0|[1-9][0-9]*)", config[field]) is None
           for field in _CONFIG_UNSIGNED_INTEGER_FIELDS):
        return "complete run provenance has invalid resolved configuration unsigned fields"
    if any(not isinstance(config.get(field), RawNumber) for field in _CONFIG_FLOAT_FIELDS):
        return "complete run provenance has invalid resolved configuration numeric fields"

    scenario = payload.get("scenario")
    if (not isinstance(scenario, dict) or
            scenario.get("name") != config.get("scenario_name") or
            scenario.get("model_version") != config.get("model_version")):
        return "complete run provenance scenario does not match the resolved configuration"
    package = payload.get("package")
    if (not isinstance(package, dict) or package.get("kind") not in ("validation", "release") or
            not _known_identity_string(package.get("id"))):
        return "complete run provenance has incomplete package identity"
    if package.get("kind") == "release":
        if not _known_identity_string(package.get("release_tag")) or package.get("id") != package.get("release_tag"):
            return "complete run provenance release identity is inconsistent"

    expected_input_status = "unchanged" if schema == "axissim.run-provenance.v1" else "exact_bytes_captured_and_used"
    input_verification = payload.get("input_verification")
    if not isinstance(input_verification, dict) or input_verification.get("status") != expected_input_status:
        return "complete run provenance does not prove input stability"
    result = payload.get("result")
    if not isinstance(result, dict) or result.get("status") != "hashed" or not _valid_sha256(result.get("sha256")):
        return "complete run provenance has no hashed result"
    return None


def parse_run_provenance(
    text: str,
    *,
    result_json_bytes: bytes | None = None,
    require_complete: bool = False,
) -> dict[str, Any]:
    record = json.loads(
        text,
        parse_int=RawNumber,
        parse_float=RawNumber,
        parse_constant=_reject_constant,
        object_pairs_hook=_object_without_duplicate_keys,
    )
    if not isinstance(record, dict):
        raise ValueError("run provenance must be a JSON object")
    payload = record.get("provenance")
    if not isinstance(payload, dict) or payload.get("schema_version") not in (
        "axissim.run-provenance.v1", "axissim.run-provenance.v2"
    ):
        raise ValueError("run provenance schema is missing or unsupported")
    status = record.get("identity_status")
    if status not in ("complete", "incomplete"):
        raise ValueError("run provenance identity status is invalid")
    if require_complete and result_json_bytes is None:
        raise ValueError("complete run provenance verification requires the result JSON bytes")
    if require_complete and status != "complete":
        raise ValueError("run provenance identity is incomplete")
    result = payload.get("result")
    if not isinstance(result, dict) or result.get("status") not in ("hashed", "unavailable"):
        raise ValueError("run provenance result identity is missing or invalid")
    result_hash = result.get("sha256")
    if result["status"] == "hashed":
        if not isinstance(result_hash, str) or re.fullmatch(r"[0-9a-f]{64}", result_hash) is None:
            raise ValueError("run result SHA-256 is missing or malformed")
    elif result_hash != "unknown":
        raise ValueError("unavailable run result identity must be marked unknown")
    input_verification = payload.get("input_verification")
    schema = payload["schema_version"]
    accepted_input_statuses = (
        ("unchanged", "changed_or_unavailable") if schema == "axissim.run-provenance.v1"
        else ("exact_bytes_captured_and_used",)
    )
    if not isinstance(input_verification, dict) or input_verification.get("status") not in accepted_input_statuses:
        raise ValueError("run input-stability status is missing or invalid")
    if status == "complete" and (
        result["status"] != "hashed" or
        input_verification["status"] not in ("unchanged", "exact_bytes_captured_and_used")
    ):
        raise ValueError("complete run provenance has incomplete result or input verification")
    if status == "complete":
        issue = _complete_identity_issue(payload, schema)
        if issue is not None:
            raise ValueError(issue)
    claimed = record.get("identity_sha256")
    if not isinstance(claimed, str) or re.fullmatch(r"[0-9a-f]{64}", claimed) is None:
        raise ValueError("run provenance identity hash is missing or malformed")
    identity_input = {"identity_status": status, "provenance": payload}
    actual = hashlib.sha256(canonical_json(identity_input).encode("utf-8")).hexdigest()
    if actual != claimed:
        raise ValueError("run provenance status/payload does not match its canonical SHA-256 identity")
    if result_json_bytes is not None and result["status"] == "hashed":
        if hashlib.sha256(result_json_bytes).hexdigest() != result_hash:
            raise ValueError("run result JSON does not match its provenance SHA-256 identity")
    return record
