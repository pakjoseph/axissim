#!/usr/bin/env python3
"""CLI contract checks for the opt-in paired GEMM evaluator."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


REQUEST = {
    "schema_id": "axissim.gemm-pair.request",
    "schema_version": 1,
    "canonicalization_id": "axissim-json-canonical-v1",
    "workload": {
        "operation": "gemm", "m": 2, "n": 3, "k": 4, "batch_count": 1,
        "input_format": "signed_i16_twos_complement",
        "accumulator_output_format": "signed_i32_twos_complement",
        "overflow": "wrap_mod_2pow32", "rounding": "none",
        "layout": "row_major_contiguous", "transpose_a": False,
        "transpose_b": False, "initial_c": "zero",
        "operand_data_identity": "shape_only_no_values_v1",
    },
    "measurement": {
        "protocol": "finite_serial_batch_v1", "repeat_count": 3,
        "release_policy": "all_at_zero", "unit_order": "ascending_index_no_overlap",
        "clock_hz": 1000000000,
    },
    "mappings": {
        "rvv": {
            "kind": "rvv_row_v1", "vlen_bits": 128, "sew_bits": 16,
            "lmul": 1, "lanes": 4, "macs_per_lane_cycle": 1,
            "accumulator_recurrence_cycles": 1, "kernel_setup_cycles": 0,
            "chunk_setup_cycles": 0,
        },
        "matrix": {
            "kind": "tiled_matrix_output_stationary_v1", "tile_m": 2,
            "tile_n": 2, "tile_k": 3, "local_capacity_bytes": 64,
            "mac_slots_per_cycle": 8, "accumulator_recurrence_cycles": 1,
            "kernel_setup_cycles": 0, "tile_setup_cycles": 0,
        },
    },
}


def canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii")


def digest(value: object) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def invoke(binary: Path, request: bytes, result_path: Path) -> subprocess.CompletedProcess[bytes]:
    request_path = result_path.with_suffix(".request.json")
    request_path.write_bytes(request)
    return subprocess.run(
        [str(binary), "study-gemm-pair", "--request", str(request_path), "--result", str(result_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit(f"missing simulator binary: {binary}")

    with tempfile.TemporaryDirectory(prefix="axissim-gemm-pair-") as temp:
        root = Path(temp)
        encoded = json.dumps(REQUEST, separators=(",", ":")).encode("ascii")
        one = root / "one.result.json"
        two = root / "two.result.json"
        run_one = invoke(binary, encoded, one)
        run_two = invoke(binary, encoded, two)
        assert run_one.returncode == 0, run_one.stderr.decode(errors="replace")
        assert run_two.returncode == 0, run_two.stderr.decode(errors="replace")
        assert not run_one.stdout and not run_two.stdout, "routine stdout must remain empty"
        one_bytes = one.read_bytes()
        assert one_bytes.endswith(b"\n") and one_bytes.count(b"\n") == 1
        assert one_bytes == two.read_bytes(), "identical request did not produce identical record bytes"
        result = json.loads(one_bytes)
        assert result["schema_id"] == "axissim.gemm-pair.result"
        assert result["schema_version"] == 1
        assert result["status"] == "complete"
        assert result["arms"]["rvv"]["workload_counts"]["useful_macs_per_unit"] == 24
        assert result["arms"]["matrix"]["workload_counts"]["useful_macs_per_unit"] == 24
        assert result["energy_status"] == "not_modeled"
        assert result["budget_status"] == "not_evaluated"
        assert result["model_coverage"]["physical_memory_actions"] == "not_modeled"
        workload_id = digest(REQUEST["workload"])
        rvv_id = digest(REQUEST["mappings"]["rvv"])
        matrix_id = digest(REQUEST["mappings"]["matrix"])
        assert result["request_sha256"] == digest(REQUEST)
        assert result["workload_id"] == workload_id
        assert result["arms"]["rvv"]["mapping_id"] == rvv_id
        assert result["arms"]["matrix"]["mapping_id"] == matrix_id
        pair_identity = {
            "schema_id": REQUEST["schema_id"],
            "schema_version": REQUEST["schema_version"],
            "workload_id": workload_id,
            "measurement": REQUEST["measurement"],
            "mappings": {"rvv": rvv_id, "matrix": matrix_id},
        }
        assert result["candidate_pair_id"] == digest(pair_identity)
        assumptions = {
            "schema_id": "axissim.gemm-pair.model-assumptions",
            "schema_version": 1,
            "workload_id": workload_id,
            "measurement": REQUEST["measurement"],
            "model_semantics": {
                "rvv": "axissim.rvv_row_v1",
                "matrix": "axissim.tiled_matrix_output_stationary_v1",
            },
            "mappings": REQUEST["mappings"],
        }
        assert result["model_assumptions_sha256"] == digest(assumptions)

        for label, mutate in (
            ("workload", lambda request: request["workload"].update(m=3)),
            ("mapping", lambda request: request["mappings"]["rvv"].update(lanes=8)),
            ("measurement", lambda request: request["measurement"].update(repeat_count=4)),
        ):
            changed_request = json.loads(json.dumps(REQUEST))
            mutate(changed_request)
            changed_result_path = root / f"changed-{label}.result.json"
            changed_run = invoke(binary, json.dumps(changed_request, separators=(",", ":")).encode("ascii"),
                                 changed_result_path)
            assert changed_run.returncode == 0, changed_run.stderr.decode(errors="replace")
            changed_result = json.loads(changed_result_path.read_bytes())
            assert changed_result["candidate_pair_id"] != result["candidate_pair_id"], label
            assert changed_result["request_sha256"] != result["request_sha256"], label
            if label == "workload":
                assert changed_result["workload_id"] != result["workload_id"]

        invalid_json = root / "invalid.result.json"
        invalid_run = invoke(binary, b"{", invalid_json)
        assert invalid_run.returncode == 2
        invalid_result = json.loads(invalid_json.read_bytes())
        assert invalid_result == {
            "errors": [{"code": "invalid_json", "path": "", "scope": "request"}],
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "invalid_json",
        }

        too_large = root / "large.result.json"
        large_run = invoke(binary, b" " * (64 * 1024 + 1), too_large)
        assert large_run.returncode == 2
        assert json.loads(too_large.read_bytes())["status"] == "input_limit"

        bad_cli = subprocess.run(
            [str(binary), "study-gemm-pair", "--request", "x", "--result", str(root / "unused"), "--trace", "z"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        assert bad_cli.returncode == 2 and not (root / "unused").exists()

        missing_result = root / "missing-input.result.json"
        missing_input = subprocess.run(
            [str(binary), "study-gemm-pair", "--request", str(root / "absent.json"),
             "--result", str(missing_result)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        assert missing_input.returncode == 4 and not missing_result.exists()

        valid_request_path = root / "io.request.json"
        valid_request_path.write_bytes(encoded)
        unwritable_target = root / "absent-directory" / "result.json"
        result_io_failure = subprocess.run(
            [str(binary), "study-gemm-pair", "--request", str(valid_request_path),
             "--result", str(unwritable_target)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        assert result_io_failure.returncode == 4 and not unwritable_target.exists()

    print("paired GEMM CLI contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
