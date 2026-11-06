#!/usr/bin/env python3
"""Render the reproducible MNIST-12 mapping-policy study for the README."""
import csv
import sys
from pathlib import Path
import matplotlib.pyplot as plt

rows = list(csv.DictReader(open(sys.argv[1], newline="")))
output = Path(sys.argv[2])
labels = [row["policy"].replace("_", "\n") for row in rows]
latency = [float(row["mean_latency_us"]) for row in rows]
energy = [float(row["it_energy_per_frame_uj"]) for row in rows]
colors = ["#68716E"] * len(rows)
for index, row in enumerate(rows):
    if row["accuracy_evidence_status"] == "external_evaluated": colors[index] = "#237B5B"
baseline = rows[0]
fig, axes = plt.subplots(1, 2, figsize=(10, 4.4), layout="constrained")
fig.patch.set_facecolor("#171918")
for axis, values, title, unit in zip(axes, (latency, energy), ("Modeled mean latency", "Modeled IT energy / frame"), ("µs", "µJ")):
    axis.set_facecolor("#202321")
    bars = axis.bar(labels, values, color=colors)
    axis.axhline(values[0], color="#8A938E", linewidth=1, linestyle="--")
    axis.set_title(title, color="#F1F0EA", loc="left", weight="bold")
    axis.set_ylabel(unit, color="#AEB4AF")
    axis.tick_params(colors="#AEB4AF")
    axis.spines[["top", "right"]].set_visible(False)
    axis.spines[["bottom", "left"]].set_color("#535B56")
    for bar, value in zip(bars, values): axis.text(bar.get_x() + bar.get_width()/2, value, f"{value:.2f}", ha="center", va="bottom", color="#F1F0EA", fontsize=9)
axes[1].text(5, energy[5] * 1.35, "controlled INT8\n98.91% externally evaluated", ha="center", color="#7CC4A5", fontsize=8)
fig.suptitle("AxisSim MNIST-12 Mapping Policies", color="#F1F0EA", x=0.01, ha="left", weight="bold")
output.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(output, dpi=180, facecolor=fig.get_facecolor())
