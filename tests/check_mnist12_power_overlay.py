#!/usr/bin/env python3
"""Verify an illustrative MNIST power overlay without changing its config."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary: Path, root: Path, result_path: Path, overrides: list[str]) -> dict[str, object]:
    command = [str(binary), "run", "configs/workloads/mnist12_profiled.conf"]
    for override in overrides:
        command.extend(["--set", override])
    command.extend(["--json", str(result_path)])
    subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
    return json.loads(result_path.read_bytes())


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if sys.version_info < (3, 10):
        raise RuntimeError("MNIST power overlay check requires Python 3.10 or newer")
    if len(sys.argv) != 3:
        raise RuntimeError("usage: check_mnist12_power_overlay.py SOURCE_ROOT AXISSIM_BINARY")

    root = Path(sys.argv[1]).resolve()
    binary = Path(sys.argv[2]).resolve()
    if not binary.is_file():
        raise RuntimeError(f"simulator binary does not exist: {binary}")

    documented_overlay = [
        "static_power_w=0.05",
        "npu_static_power_per_mac_w=0.00003",
        "rvv_static_power_per_lane_w=0.002",
        "sram_static_power_per_kib_w=0.0001",
        "dram_interface_static_power_w=0.02",
        "it_power_budget_w=0.23",
        "pue=1.5",
    ]
    with tempfile.TemporaryDirectory(prefix="axissim-mnist-overlay-") as directory:
        folder = Path(directory)
        legacy = run(binary, root, folder / "legacy.json", [])
        overlaid = run(binary, root, folder / "overlay.json", documented_overlay)

    legacy_assumptions = legacy["assumptions"]
    legacy_power = legacy["power"]
    require(legacy_assumptions["it_power_budget_w"] == 0.0, "legacy MNIST budget default changed")
    require(legacy_power["modeled_static_w"] == 0.0, "legacy MNIST static-power default changed")
    require(legacy_power["budget_w"] == 0.0, "legacy MNIST must keep budget enforcement disabled")
    require(legacy["schema_version"] == "1.2", "legacy MNIST output schema changed")

    overlay_assumptions = overlaid["assumptions"]
    overlay_power = overlaid["power"]
    expected_assumptions = {
        "base_static_power_w": 0.05,
        "npu_static_power_per_mac_w": 0.00003,
        "rvv_static_power_per_lane_w": 0.002,
        "sram_static_power_per_kib_w": 0.0001,
        "dram_interface_static_power_w": 0.02,
        "pue": 1.5,
    }
    for key, value in expected_assumptions.items():
        require(overlay_assumptions[key] == value, f"study overlay did not apply documented assumption {key}")
    require(overlay_assumptions["it_power_budget_w"] == 0.23, "study overlay did not apply the 0.23 W reference")
    require(overlay_power["modeled_static_w"] == 0.12952, "study overlay did not use documented static-power coefficients")
    require(overlay_power["budget_w"] == 0.23 and overlay_power["feasible"], "MNIST overlay budget result changed")
    require(overlay_power["average_facility_w"] == 0.194511, "PUE must affect facility power, not modeled IT power")
    require(overlaid["schema_version"] == "1.2", "overlay output schema changed")
    require(legacy["latency"] == overlaid["latency"], "power overlay changed modeled latency")
    require(legacy["timing"] == overlaid["timing"], "power overlay changed makespan")
    require(legacy["traffic"] == overlaid["traffic"], "power overlay changed workload traffic")
    for key in ("per_frame_uj", "vector_pj", "npu_arithmetic_pj", "dram_pj", "dma_pj"):
        require(legacy["energy"][key] == overlaid["energy"][key], f"power overlay changed dynamic metric {key}")

    print("PASS MNIST-12 legacy defaults remain zero; documented study-only power overlay is feasible and timing-neutral")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"MNIST power overlay check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
