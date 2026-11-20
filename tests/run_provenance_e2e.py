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
from provenance_contract import parse_run_provenance


def run(binary: pathlib.Path, config: pathlib.Path, output: pathlib.Path, *overrides: str) -> dict:
    trace = output.with_suffix(".trace.csv")
    command = [str(binary), "run", str(config), "--json", str(output), "--trace", str(trace)]
    for assignment in overrides:
        command.extend(["--set", assignment])
    subprocess.run(command, check=True, capture_output=True, text=True)
    result = json.loads(output.read_text(encoding="utf-8"))
    sidecar_path = pathlib.Path(str(output) + ".provenance.json")
    provenance = parse_run_provenance(sidecar_path.read_text(encoding="utf-8"))
    if not trace.is_file() or trace.stat().st_size == 0:
        raise AssertionError("run did not emit a task trace")
    if result.get("schema_version") != "1.2":
        raise AssertionError("legacy result JSON schema changed")
    if provenance.get("provenance", {}).get("schema_version") != "axissim.run-provenance.v1":
        raise AssertionError("unsupported provenance schema")
    return {
        "result": result,
        "provenance": provenance,
        "sidecar_text": sidecar_path.read_text(encoding="utf-8"),
    }


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

        baseline = json.loads((source / "experiments/baseline_result.json").read_text(encoding="utf-8"))
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
    print("run provenance sidecar E2E passed: reproducibility, executable/config identity, override, transitive include, and schema 1.2 parity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
