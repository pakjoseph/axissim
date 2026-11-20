"""Strict parser and verifier for AxisSim's run-provenance sidecar contract."""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any


class RawNumber(str):
    """JSON number preserving the exact lexical form emitted by C++."""


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


def parse_run_provenance(text: str, *, require_complete: bool = False) -> dict[str, Any]:
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
    if not isinstance(payload, dict) or payload.get("schema_version") != "axissim.run-provenance.v1":
        raise ValueError("run provenance schema is missing or unsupported")
    status = record.get("identity_status")
    if status not in ("complete", "incomplete"):
        raise ValueError("run provenance identity status is invalid")
    if require_complete and status != "complete":
        raise ValueError("run provenance identity is incomplete")
    claimed = record.get("identity_sha256")
    if not isinstance(claimed, str) or re.fullmatch(r"[0-9a-f]{64}", claimed) is None:
        raise ValueError("run provenance identity hash is missing or malformed")
    actual = hashlib.sha256(canonical_json(payload).encode("utf-8")).hexdigest()
    if actual != claimed:
        raise ValueError("run provenance payload does not match its canonical SHA-256 identity")
    return record
