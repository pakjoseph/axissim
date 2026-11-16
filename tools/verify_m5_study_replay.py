#!/usr/bin/env python3
"""Verify M5 candidate results and retained traces across simulator builds.

Only top-level candidate_id and simulator_build_identity fields in requests
and results, and those fields in the JSONL provenance header, are ignored.
Task rows and nested fields are compared exactly. The M5 output directory also
contains identity-bearing filenames and hashes, so raw artifact-bundle identity
is not the compatibility criterion.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path, PureWindowsPath
from typing import Any


IDENTITY_FIELDS = {"candidate_id", "simulator_build_identity"}
EXPECTED_CANDIDATES = 168
EXPECTED_RETAINED_TRACES = 88
EXPECTED_REFERENCE_ARTIFACTS = 555
ACCEPTED_M5_MANIFEST_ID = "52cb0f4be4afdce42bab3ef8e6dec036a7eefa6b6429319306ba5feaa7a92256"
ACCEPTED_M5_SUMMARY_SHA256 = "b632b2b53258501c7958b353fc174afefca3923f8618e0f0a00ca71b96f12b3e"
ACCEPTED_M5_SIMULATOR_ID = "6be8f336e12c75eca1dba46d61c00302a5d4b4881e8ce10315dd48c6a659107e"
EXPECTED_TRACE_INDEX_SCHEMA = "axissim.study-trace-index.v1"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
NONCANONICAL_M5_FILES = {
    "study-summary.json",
    "host-diagnostics/execution.jsonl",
    "host-diagnostics/resource-usage.json",
    # The M5 `reproduce` command writes its own run-specific check outside
    # the canonical artifact inventory. It is absent from the accepted source.
    "reproduction-check.json",
}


class VerificationError(RuntimeError):
    pass


def canonical_json(value: Any) -> str:
    def encode(item: Any) -> str:
        if item is None:
            return "null"
        if item is True:
            return "true"
        if item is False:
            return "false"
        if isinstance(item, int):
            return str(item)
        if isinstance(item, Decimal):
            if not item.is_finite():
                raise VerificationError("non-finite JSON number")
            if item.is_zero():
                return "-0.0" if item.is_signed() else "0.0"
            sign, raw_digits, exponent = item.as_tuple()
            digits = list(raw_digits)
            while len(digits) > 1 and digits[-1] == 0:
                digits.pop()
                exponent += 1
            adjusted_exponent = exponent + len(digits) - 1
            mantissa = str(digits[0]) + "." + "".join(str(digit) for digit in digits[1:])
            if len(digits) == 1:
                mantissa += "0"
            return ("-" if sign else "") + mantissa + "e" + str(adjusted_exponent)
        if isinstance(item, float):
            if not math.isfinite(item):
                raise VerificationError("non-finite JSON number")
            # Values parsed from files use Decimal. This branch is retained for
            # direct callers and test fixtures.
            return json.dumps(item, allow_nan=False, separators=(",", ":"))
        if isinstance(item, str):
            return json.dumps(item, ensure_ascii=False)
        if isinstance(item, list):
            return "[" + ",".join(encode(child) for child in item) + "]"
        if isinstance(item, dict):
            entries = sorted(item.items())
            return "{" + ",".join(encode(str(key)) + ":" + encode(child) for key, child in entries) + "}"
        raise VerificationError(f"unsupported JSON value type: {type(item).__name__}")

    return encode(value)


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in pairs:
        if key in output:
            raise VerificationError(f"duplicate JSON object key: {key}")
        output[key] = value
    return output


def _parse_finite_float(token: str) -> Decimal:
    try:
        value = Decimal(token)
    except InvalidOperation as exc:
        raise VerificationError(f"invalid JSON number: {token}") from exc
    if not value.is_finite():
        raise VerificationError(f"non-finite JSON number: {token}")
    return value


def _reject_json_constant(token: str) -> None:
    raise VerificationError(f"non-standard JSON number: {token}")


def without_build_identity(value: Any) -> Any:
    if isinstance(value, dict):
        # Candidate and executable identities are build-dependent only at the
        # top level of requests/results and in the JSONL provenance header.
        # Nested fields remain part of the exact comparison contract.
        return {
            key: child for key, child in sorted(value.items())
            if key not in IDENTITY_FIELDS
        }
    return value


def read_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=_unique_object,
                             parse_float=_parse_finite_float, parse_constant=_reject_json_constant)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise VerificationError(f"cannot read JSON {path}: {exc}") from exc


def contained_study_path(root: Path, relative: str) -> Path:
    if (not isinstance(relative, str) or not relative or "\x00" in relative
            or "\\" in relative or Path(relative).is_absolute()
            or PureWindowsPath(relative).is_absolute() or PureWindowsPath(relative).drive):
        raise VerificationError(f"invalid relative artifact path: {relative!r}")
    if any(component in ("", ".", "..") for component in relative.split("/")):
        raise VerificationError(f"non-canonical artifact path: {relative}")
    base = root.resolve()
    candidate = base / relative
    current = base
    for component in Path(relative).parts:
        current = current / component
        if current.is_symlink():
            raise VerificationError(f"symbolic link is not allowed in artifact path: {relative}")
    path = candidate.resolve()
    if not path.is_relative_to(base):
        raise VerificationError(f"artifact escapes study directory: {relative}")
    return path


def artifact_integrity(root: Path) -> tuple[int, str]:
    summary_path = contained_study_path(root, "study-summary.json")
    summary = read_json(summary_path)
    if not isinstance(summary, dict):
        raise VerificationError(f"{summary_path} must contain a JSON object")
    records = summary.get("canonical_artifacts")
    if not isinstance(records, list):
        raise VerificationError(f"{summary_path} has no canonical_artifacts list")
    seen_paths: set[str] = set()
    for item in records:
        if not isinstance(item, dict):
            raise VerificationError(f"invalid artifact record in {summary_path}")
        relative = item.get("path")
        if not isinstance(relative, str) or relative in seen_paths:
            raise VerificationError(f"invalid artifact path record in {summary_path}")
        seen_paths.add(relative)
        expected_bytes = item.get("bytes")
        expected_sha = item.get("sha256")
        if (not isinstance(expected_bytes, int) or isinstance(expected_bytes, bool) or expected_bytes < 0
                or not isinstance(expected_sha, str) or not SHA256_RE.fullmatch(expected_sha)):
            raise VerificationError(f"invalid artifact hash/size record for {relative}")
        path = contained_study_path(root, relative)
        try:
            data = path.read_bytes()
        except OSError as exc:
            raise VerificationError(f"missing canonical artifact {relative}: {exc}") from exc
        if len(data) != expected_bytes or hashlib.sha256(data).hexdigest() != expected_sha:
            raise VerificationError(f"canonical artifact integrity mismatch: {relative}")
    actual_canonical_paths = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.relative_to(root).as_posix() not in NONCANONICAL_M5_FILES
    }
    if actual_canonical_paths != seen_paths:
        missing = sorted(actual_canonical_paths - seen_paths)
        unexpected = sorted(seen_paths - actual_canonical_paths)
        raise VerificationError(f"canonical artifact file set mismatch: unlisted={missing}, absent={unexpected}")
    return len(records), hashlib.sha256(summary_path.read_bytes()).hexdigest()


def load_requests(root: Path, trace_requests: bool = False,
                  expected_build_identity: str | None = None) -> dict[str, tuple[str, dict[str, Any]]]:
    request_dir = root / "requests"
    output: dict[str, tuple[str, dict[str, Any]]] = {}
    seen_ids: set[str] = set()
    for path in sorted(request_dir.glob("*.json")):
        is_trace = path.name.endswith(".trace.json")
        if is_trace != trace_requests:
            continue
        relative_path = path.relative_to(root).as_posix()
        path = contained_study_path(root, relative_path)
        request = read_json(path)
        if not isinstance(request, dict):
            raise VerificationError(f"request must be a JSON object: {path}")
        candidate_id = request.get("candidate_id")
        if not isinstance(candidate_id, str) or not SHA256_RE.fullmatch(candidate_id):
            raise VerificationError(f"request lacks candidate_id: {path}")
        if expected_build_identity is not None and request.get("simulator_build_identity") != expected_build_identity:
            raise VerificationError(f"request build identity does not match study summary: {path}")
        expected_name = f"{candidate_id}.trace.json" if trace_requests else f"{candidate_id}.json"
        if path.name != expected_name:
            raise VerificationError(f"request filename does not match candidate_id: {path.name}")
        if candidate_id in seen_ids:
            raise VerificationError(f"duplicate candidate_id in requests: {candidate_id}")
        seen_ids.add(candidate_id)
        normalized = canonical_json(without_build_identity(request))
        if normalized in output:
            raise VerificationError(f"duplicate canonical request: {path}")
        output[normalized] = (candidate_id, request)
    return output


def load_trace_index(root: Path) -> tuple[dict[str, dict[str, Any]], dict[str, int]]:
    index = read_json(contained_study_path(root, "trace-index.json"))
    if not isinstance(index, dict) or index.get("schema_id") != EXPECTED_TRACE_INDEX_SCHEMA:
        raise VerificationError(f"unsupported trace index schema in {root}")
    expected_index_keys = {"schema_id", "records", "retained_trace_bytes", "trace_cap_bytes"}
    if set(index) != expected_index_keys:
        raise VerificationError(f"unexpected trace index fields in {root}")
    for field in ("retained_trace_bytes", "trace_cap_bytes"):
        if not isinstance(index.get(field), int) or isinstance(index[field], bool) or index[field] < 0:
            raise VerificationError(f"invalid trace index {field} in {root}")
    records = index.get("records")
    if not isinstance(records, list):
        raise VerificationError(f"invalid trace index in {root}")
    output: dict[str, dict[str, Any]] = {}
    for item in records:
        if not isinstance(item, dict):
            raise VerificationError(f"invalid trace index record in {root}")
        expected_record_keys = {"candidate_id", "status", "reason", "trace_path", "record_count", "bytes", "sha256"}
        if set(item) != expected_record_keys:
            raise VerificationError(f"unexpected trace index record fields in {root}")
        candidate_id = item.get("candidate_id")
        if not isinstance(candidate_id, str) or not SHA256_RE.fullmatch(candidate_id):
            raise VerificationError(f"invalid trace candidate_id in {root}")
        if candidate_id in output:
            raise VerificationError(f"duplicate trace index candidate_id in {root}: {candidate_id}")
        if item.get("status") != "written":
            raise VerificationError(f"unexpected trace status for candidate {candidate_id}: {item.get('status')!r}")
        if item.get("reason") is not None:
            raise VerificationError(f"unexpected trace omission reason for candidate {candidate_id}")
        if item.get("trace_path") != f"traces/{candidate_id}.jsonl":
            raise VerificationError(f"unexpected trace artifact location for candidate {candidate_id}")
        if (not isinstance(item.get("record_count"), int) or isinstance(item.get("record_count"), bool)
                or item["record_count"] < 0 or not isinstance(item.get("bytes"), int)
                or isinstance(item.get("bytes"), bool) or item["bytes"] < 0
                or not isinstance(item.get("sha256"), str) or not SHA256_RE.fullmatch(item["sha256"])):
            raise VerificationError(f"invalid trace size/hash metadata for candidate {candidate_id}")
        output[candidate_id] = item
    if index["retained_trace_bytes"] != sum(item["bytes"] for item in output.values()):
        raise VerificationError(f"trace index retained byte total is inconsistent in {root}")
    return output, {"trace_cap_bytes": index["trace_cap_bytes"]}


def read_jsonl(path: Path) -> list[Any]:
    try:
        rows = [json.loads(line, object_pairs_hook=_unique_object,
                           parse_float=_parse_finite_float, parse_constant=_reject_json_constant)
                for line in path.read_text(encoding="utf-8").splitlines()]
        if any(not isinstance(row, dict) for row in rows):
            raise VerificationError(f"JSONL trace contains a non-object record: {path}")
        return rows
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise VerificationError(f"cannot read JSONL trace {path}: {exc}") from exc


def compare_candidate_results(reference: Path, replay: Path, ref_requests: dict[str, tuple[str, dict[str, Any]]],
                              new_requests: dict[str, tuple[str, dict[str, Any]]],
                              reference_build_identity: str, replay_build_identity: str) -> None:
    if ref_requests.keys() != new_requests.keys():
        raise VerificationError("candidate request sets differ after build-identity normalization")
    if len(ref_requests) != EXPECTED_CANDIDATES:
        raise VerificationError(f"expected {EXPECTED_CANDIDATES} candidates, found {len(ref_requests)}")

    for request_key in sorted(ref_requests):
        ref_id, ref_request = ref_requests[request_key]
        new_id, new_request = new_requests[request_key]
        # This equality removes only the identities necessarily tied to a build.
        if canonical_json(without_build_identity(ref_request)) != canonical_json(without_build_identity(new_request)):
            raise VerificationError(f"candidate request changed beyond build identity: {ref_id}")
        ref_result = read_json(contained_study_path(reference, f"results/{ref_id}.json"))
        new_result = read_json(contained_study_path(replay, f"results/{new_id}.json"))
        if not isinstance(ref_result, dict) or not isinstance(new_result, dict):
            raise VerificationError(f"candidate result must be a JSON object: {ref_id}")
        if ref_result.get("candidate_id") != ref_id or new_result.get("candidate_id") != new_id:
            raise VerificationError("result candidate identity does not match its request")
        if ref_result.get("simulator_build_identity") != reference_build_identity:
            raise VerificationError(f"reference result build identity does not match study summary: {ref_id}")
        if new_result.get("simulator_build_identity") != replay_build_identity:
            raise VerificationError(f"replay result build identity does not match study summary: {new_id}")
        if canonical_json(without_build_identity(ref_result)) != canonical_json(without_build_identity(new_result)):
            raise VerificationError(f"candidate result differs beyond build identity: {ref_id}")


def compare_retained_traces(reference: Path, replay: Path,
                            ref_requests: dict[str, tuple[str, dict[str, Any]]],
                            new_requests: dict[str, tuple[str, dict[str, Any]]],
                            reference_build_identity: str, replay_build_identity: str) -> tuple[int, int]:
    ref_index, ref_index_metadata = load_trace_index(reference)
    new_index, new_index_metadata = load_trace_index(replay)
    if ref_index_metadata != new_index_metadata:
        raise VerificationError("trace index capacity metadata differs")
    trace_count = 0
    row_count = 0
    for request_key in sorted(ref_requests.keys() & new_requests.keys()):
        ref_id = ref_requests[request_key][0]
        new_id = new_requests[request_key][0]
        left = ref_index.get(ref_id)
        right = new_index.get(new_id)
        if left is None or right is None:
            if left != right:
                raise VerificationError(f"retained-trace selection changed for candidate {ref_id}")
            continue
        if (left.get("status"), left.get("record_count"), left.get("reason")) != (
            right.get("status"), right.get("record_count"), right.get("reason")
        ):
            raise VerificationError(f"trace metadata changed beyond provenance for candidate {ref_id}")
        if left.get("status") != "written":
            continue
        left_path = contained_study_path(reference, left["trace_path"])
        right_path = contained_study_path(replay, right["trace_path"])
        left_data = left_path.read_bytes()
        right_data = right_path.read_bytes()
        if len(left_data) != left["bytes"] or hashlib.sha256(left_data).hexdigest() != left["sha256"]:
            raise VerificationError(f"reference trace index hash/size mismatch for candidate {ref_id}")
        if len(right_data) != right["bytes"] or hashlib.sha256(right_data).hexdigest() != right["sha256"]:
            raise VerificationError(f"replay trace index hash/size mismatch for candidate {new_id}")
        left_rows = read_jsonl(left_path)
        right_rows = read_jsonl(right_path)
        # A JSONL trace begins with one provenance header outside the indexed
        # task-record count.
        if len(left_rows) != left.get("record_count") + 1 or len(right_rows) != right.get("record_count") + 1:
            raise VerificationError(f"trace record count does not match index for candidate {ref_id}")
        if not left_rows or not right_rows or not isinstance(left_rows[0], dict) or not isinstance(right_rows[0], dict):
            raise VerificationError(f"trace is missing a provenance header for candidate {ref_id}")
        if left_rows[0].get("candidate_id") != ref_id or right_rows[0].get("candidate_id") != new_id:
            raise VerificationError(f"trace header candidate identity does not match its request: {ref_id}")
        if left_rows[0].get("simulator_build_identity") != reference_build_identity:
            raise VerificationError(f"reference trace build identity does not match study summary: {ref_id}")
        if right_rows[0].get("simulator_build_identity") != replay_build_identity:
            raise VerificationError(f"replay trace build identity does not match study summary: {new_id}")
        headers_equal = canonical_json(without_build_identity(left_rows[0])) == canonical_json(without_build_identity(right_rows[0]))
        task_rows_equal = canonical_json(left_rows[1:]) == canonical_json(right_rows[1:])
        if not headers_equal or not task_rows_equal:
            raise VerificationError(f"task trace differs beyond build identity for candidate {ref_id}")
        if left["bytes"] != right["bytes"]:
            raise VerificationError(f"task trace byte counts differ for candidate {ref_id}")
        trace_count += 1
        row_count += int(left.get("record_count"))

    if trace_count != EXPECTED_RETAINED_TRACES:
        raise VerificationError(f"expected {EXPECTED_RETAINED_TRACES} retained traces, compared {trace_count}")
    return trace_count, row_count


def verify(reference: Path, replay: Path) -> dict[str, Any]:
    reference = reference.resolve()
    replay = replay.resolve()
    ref_summary = read_json(contained_study_path(reference, "study-summary.json"))
    replay_summary = read_json(contained_study_path(replay, "study-summary.json"))
    if not isinstance(ref_summary, dict) or not isinstance(replay_summary, dict):
        raise VerificationError("study summaries must be JSON objects")
    if ref_summary.get("manifest_id") != replay_summary.get("manifest_id"):
        raise VerificationError("study manifest identities differ")
    if ref_summary.get("candidate_count_selected") != EXPECTED_CANDIDATES:
        raise VerificationError(f"accepted reference does not contain the expected {EXPECTED_CANDIDATES}-candidate study")
    if ref_summary.get("manifest_id") != ACCEPTED_M5_MANIFEST_ID:
        raise VerificationError("reference manifest identity does not match the accepted M5 study")
    ref_summary_sha = hashlib.sha256((reference / "study-summary.json").read_bytes()).hexdigest()
    if ref_summary_sha != ACCEPTED_M5_SUMMARY_SHA256:
        raise VerificationError("reference study-summary SHA-256 does not match the accepted M5 bundle")
    if ref_summary.get("simulator_build_identity") != ACCEPTED_M5_SIMULATOR_ID:
        raise VerificationError("reference simulator identity does not match the accepted M5 build")
    if replay_summary.get("candidate_count_selected") != EXPECTED_CANDIDATES:
        raise VerificationError("replay did not select all 168 M5 candidates")
    reference_build_identity = ref_summary.get("simulator_build_identity")
    replay_build_identity = replay_summary.get("simulator_build_identity")
    if (not isinstance(reference_build_identity, str) or not SHA256_RE.fullmatch(reference_build_identity)
            or not isinstance(replay_build_identity, str) or not SHA256_RE.fullmatch(replay_build_identity)):
        raise VerificationError("study summary has an invalid simulator build identity")

    ref_count, ref_summary_sha = artifact_integrity(reference)
    replay_count, _ = artifact_integrity(replay)
    if ref_count != EXPECTED_REFERENCE_ARTIFACTS:
        raise VerificationError(f"expected {EXPECTED_REFERENCE_ARTIFACTS} reference artifacts, found {ref_count}")
    if replay_count != EXPECTED_REFERENCE_ARTIFACTS:
        raise VerificationError(f"expected {EXPECTED_REFERENCE_ARTIFACTS} replay artifacts, found {replay_count}")

    ref_requests = load_requests(reference, expected_build_identity=reference_build_identity)
    replay_requests = load_requests(replay, expected_build_identity=replay_build_identity)
    compare_candidate_results(reference, replay, ref_requests, replay_requests,
                              reference_build_identity, replay_build_identity)
    trace_requests = load_requests(reference, trace_requests=True,
                                   expected_build_identity=reference_build_identity)
    replay_trace_requests = load_requests(replay, trace_requests=True,
                                          expected_build_identity=replay_build_identity)
    if trace_requests.keys() != replay_trace_requests.keys():
        raise VerificationError("trace request sets differ beyond build identity")
    ref_trace_ids = {candidate_id for candidate_id, _ in trace_requests.values()}
    replay_trace_ids = {candidate_id for candidate_id, _ in replay_trace_requests.values()}
    if set(load_trace_index(reference)[0]) != ref_trace_ids:
        raise VerificationError("reference trace index does not match its trace request set")
    if set(load_trace_index(replay)[0]) != replay_trace_ids:
        raise VerificationError("replay trace index does not match its trace request set")
    traces, trace_rows = compare_retained_traces(reference, replay, ref_requests, replay_requests,
                                                 reference_build_identity, replay_build_identity)

    return {
        "schema_id": "axissim.m5-replay-identity-normalized.v1",
        "manifest_id": ref_summary["manifest_id"],
        "reference_simulator_build_identity": ref_summary.get("simulator_build_identity"),
        "replay_simulator_build_identity": replay_summary.get("simulator_build_identity"),
        "reference_study_summary_sha256": ref_summary_sha,
        "reference_artifact_integrity": f"{ref_count}/{ref_count}",
        "replay_artifact_integrity": f"{replay_count}/{replay_count}",
        "candidate_requests_matched": len(ref_requests),
        "candidate_results_equal_after_build_identity_normalization": len(ref_requests),
        "trace_requests_matched": len(trace_requests),
        "retained_task_traces_equal_after_build_identity_normalization": traces,
        "retained_task_trace_rows_equal": trace_rows,
        "raw_artifact_bytes_expected_to_match": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path, help="preserved accepted M5 study directory")
    parser.add_argument("--replay", required=True, type=Path, help="new M5 study replay directory")
    parser.add_argument("--report", type=Path, help="optional path for the JSON verification report")
    args = parser.parse_args()
    try:
        report = verify(args.reference, args.replay)
    except VerificationError as exc:
        print(f"M5 replay verification failed: {exc}", file=sys.stderr)
        return 1
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
