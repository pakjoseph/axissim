#!/usr/bin/env python3
"""Verify M5 candidate results and retained traces across simulator builds.

Only candidate_id and simulator_build_identity are removed when requests,
results, or trace rows are compared. The M5 output directory also contains
identity-bearing filenames and hashes, so raw artifact-bundle identity is not
the compatibility criterion.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


IDENTITY_FIELDS = {"candidate_id", "simulator_build_identity"}
EXPECTED_CANDIDATES = 168
EXPECTED_RETAINED_TRACES = 88
EXPECTED_REFERENCE_ARTIFACTS = 555
ACCEPTED_M5_MANIFEST_ID = "52cb0f4be4afdce42bab3ef8e6dec036a7eefa6b6429319306ba5feaa7a92256"
ACCEPTED_M5_SUMMARY_SHA256 = "b632b2b53258501c7958b353fc174afefca3923f8618e0f0a00ca71b96f12b3e"
ACCEPTED_M5_SIMULATOR_ID = "6be8f336e12c75eca1dba46d61c00302a5d4b4881e8ce10315dd48c6a659107e"


class VerificationError(RuntimeError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def without_build_identity(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: without_build_identity(child)
            for key, child in sorted(value.items())
            if key not in IDENTITY_FIELDS
        }
    if isinstance(value, list):
        return [without_build_identity(child) for child in value]
    return value


def read_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot read JSON {path}: {exc}") from exc


def contained_study_path(root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or Path(relative).is_absolute():
        raise VerificationError(f"invalid relative artifact path: {relative!r}")
    base = root.resolve()
    path = (base / relative).resolve()
    if not path.is_relative_to(base):
        raise VerificationError(f"artifact escapes study directory: {relative}")
    return path


def artifact_integrity(root: Path) -> tuple[int, str]:
    summary_path = root / "study-summary.json"
    summary = read_json(summary_path)
    records = summary.get("canonical_artifacts")
    if not isinstance(records, list):
        raise VerificationError(f"{summary_path} has no canonical_artifacts list")
    for item in records:
        relative = item.get("path")
        if not isinstance(relative, str):
            raise VerificationError(f"invalid artifact path record in {summary_path}")
        path = contained_study_path(root, relative)
        try:
            data = path.read_bytes()
        except OSError as exc:
            raise VerificationError(f"missing canonical artifact {relative}: {exc}") from exc
        if len(data) != item.get("bytes") or hashlib.sha256(data).hexdigest() != item.get("sha256"):
            raise VerificationError(f"canonical artifact integrity mismatch: {relative}")
    return len(records), hashlib.sha256(summary_path.read_bytes()).hexdigest()


def load_requests(root: Path, trace_requests: bool = False) -> dict[str, tuple[str, dict[str, Any]]]:
    request_dir = root / "requests"
    output: dict[str, tuple[str, dict[str, Any]]] = {}
    for path in sorted(request_dir.glob("*.json")):
        is_trace = path.name.endswith(".trace.json")
        if is_trace != trace_requests:
            continue
        request = read_json(path)
        candidate_id = request.get("candidate_id")
        if not isinstance(candidate_id, str):
            raise VerificationError(f"request lacks candidate_id: {path}")
        normalized = canonical_json(without_build_identity(request))
        if normalized in output:
            raise VerificationError(f"duplicate canonical request: {path}")
        output[normalized] = (candidate_id, request)
    return output


def load_trace_index(root: Path) -> dict[str, dict[str, Any]]:
    index = read_json(root / "trace-index.json")
    records = index.get("records")
    if not isinstance(records, list):
        raise VerificationError(f"invalid trace index in {root}")
    return {item["candidate_id"]: item for item in records}


def compare_candidate_results(reference: Path, replay: Path, ref_requests: dict[str, tuple[str, dict[str, Any]]],
                              new_requests: dict[str, tuple[str, dict[str, Any]]]) -> None:
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
        ref_result = read_json(reference / "results" / f"{ref_id}.json")
        new_result = read_json(replay / "results" / f"{new_id}.json")
        if ref_result.get("candidate_id") != ref_id or new_result.get("candidate_id") != new_id:
            raise VerificationError("result candidate identity does not match its request")
        if canonical_json(without_build_identity(ref_result)) != canonical_json(without_build_identity(new_result)):
            raise VerificationError(f"candidate result differs beyond build identity: {ref_id}")


def compare_retained_traces(reference: Path, replay: Path,
                            ref_requests: dict[str, tuple[str, dict[str, Any]]],
                            new_requests: dict[str, tuple[str, dict[str, Any]]]) -> tuple[int, int]:
    ref_index = load_trace_index(reference)
    new_index = load_trace_index(replay)
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
        if (left.get("status"), left.get("record_count"), left.get("bytes"), left.get("reason")) != (
            right.get("status"), right.get("record_count"), right.get("bytes"), right.get("reason")
        ):
            raise VerificationError(f"trace metadata changed beyond provenance for candidate {ref_id}")
        if left.get("status") != "written":
            continue
        left_path = contained_study_path(reference, left["trace_path"])
        right_path = contained_study_path(replay, right["trace_path"])
        left_rows = [json.loads(line) for line in left_path.read_text(encoding="utf-8").splitlines()]
        right_rows = [json.loads(line) for line in right_path.read_text(encoding="utf-8").splitlines()]
        # A JSONL trace begins with one provenance header outside the indexed
        # task-record count.
        if len(left_rows) != left.get("record_count") + 1 or len(right_rows) != right.get("record_count") + 1:
            raise VerificationError(f"trace record count does not match index for candidate {ref_id}")
        if canonical_json(without_build_identity(left_rows)) != canonical_json(without_build_identity(right_rows)):
            raise VerificationError(f"task trace differs beyond build identity for candidate {ref_id}")
        trace_count += 1
        row_count += int(left.get("record_count"))

    if trace_count != EXPECTED_RETAINED_TRACES:
        raise VerificationError(f"expected {EXPECTED_RETAINED_TRACES} retained traces, compared {trace_count}")
    return trace_count, row_count


def verify(reference: Path, replay: Path) -> dict[str, Any]:
    reference = reference.resolve()
    replay = replay.resolve()
    ref_summary = read_json(reference / "study-summary.json")
    replay_summary = read_json(replay / "study-summary.json")
    if ref_summary.get("manifest_id") != replay_summary.get("manifest_id"):
        raise VerificationError("study manifest identities differ")
    if ref_summary.get("candidate_count_selected") != EXPECTED_CANDIDATES:
        raise VerificationError("accepted reference is not the 168-candidate M5 study")
    if ref_summary.get("manifest_id") != ACCEPTED_M5_MANIFEST_ID:
        raise VerificationError("reference manifest identity does not match the accepted M5 study")
    ref_summary_sha = hashlib.sha256((reference / "study-summary.json").read_bytes()).hexdigest()
    if ref_summary_sha != ACCEPTED_M5_SUMMARY_SHA256:
        raise VerificationError("reference study-summary SHA-256 does not match the accepted M5 bundle")
    if ref_summary.get("simulator_build_identity") != ACCEPTED_M5_SIMULATOR_ID:
        raise VerificationError("reference simulator identity does not match the accepted M5 build")
    if replay_summary.get("candidate_count_selected") != EXPECTED_CANDIDATES:
        raise VerificationError("replay did not select all 168 M5 candidates")

    ref_count, ref_summary_sha = artifact_integrity(reference)
    replay_count, _ = artifact_integrity(replay)
    if ref_count != EXPECTED_REFERENCE_ARTIFACTS:
        raise VerificationError(f"expected {EXPECTED_REFERENCE_ARTIFACTS} reference artifacts, found {ref_count}")
    if replay_count != EXPECTED_REFERENCE_ARTIFACTS:
        raise VerificationError(f"expected {EXPECTED_REFERENCE_ARTIFACTS} replay artifacts, found {replay_count}")

    ref_requests = load_requests(reference)
    replay_requests = load_requests(replay)
    compare_candidate_results(reference, replay, ref_requests, replay_requests)
    trace_requests = load_requests(reference, trace_requests=True)
    replay_trace_requests = load_requests(replay, trace_requests=True)
    if trace_requests.keys() != replay_trace_requests.keys():
        raise VerificationError("trace request sets differ beyond build identity")
    traces, trace_rows = compare_retained_traces(reference, replay, ref_requests, replay_requests)

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
