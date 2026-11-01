#!/usr/bin/env python3
"""Render an AxisSim one-parameter sweep CSV as a latency/energy figure."""

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def load_rows(path: Path) -> list[dict[str, float | str]]:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError(f"No sweep rows found in {path}")
    numeric_fields = {
        "value", "mean_latency_us", "p99_latency_us", "deadline_misses",
        "dram_traffic_mb", "energy_per_frame_uj",
    }
    return [
        {key: float(value) if key in numeric_fields else value for key, value in row.items()}
        for row in rows
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="CSV emitted by `axissim sweep`")
    parser.add_argument("--output", type=Path, required=True, help="Output PNG or PDF path")
    args = parser.parse_args()

    rows = load_rows(args.input)
    parameter = str(rows[0]["parameter"])
    x = [float(row["value"]) for row in rows]
    mean = [float(row["mean_latency_us"]) for row in rows]
    p99 = [float(row["p99_latency_us"]) for row in rows]
    energy = [float(row["energy_per_frame_uj"]) for row in rows]

    figure, (latency_axis, energy_axis) = plt.subplots(1, 2, figsize=(10, 3.8), constrained_layout=True)
    latency_axis.plot(x, mean, marker="o", label="mean")
    latency_axis.plot(x, p99, marker="s", linestyle="--", label="P99")
    latency_axis.set(title="Latency tradeoff", xlabel=parameter, ylabel="latency (µs)")
    latency_axis.grid(alpha=0.25)
    latency_axis.legend()

    energy_axis.plot(x, energy, marker="o", color="#b84a62")
    energy_axis.set(title="Energy tradeoff", xlabel=parameter, ylabel="energy/frame (µJ)")
    energy_axis.grid(alpha=0.25)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=180)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
