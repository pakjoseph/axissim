#!/usr/bin/env python3
"""Summarize AxisSim's emitted MNIST-12 profile study CSV."""
import csv
import sys

rows = list(csv.DictReader(open(sys.argv[1], newline="")))
fastest = min(rows, key=lambda row: float(row["mean_latency_us"]))
baseline = next(row for row in rows if row["rvv_lanes"] == "8" and row["npu_macs"] == "1024" and row["dram_bandwidth_gbps"] == "20" and row["npu_arithmetic_energy_scale"] == "1")
print("fastest," + ",".join(fastest[key] for key in ("rvv_lanes", "npu_macs", "dram_bandwidth_gbps", "npu_arithmetic_energy_scale", "mean_latency_us", "energy_per_frame_uj")))
print("baseline," + ",".join(baseline[key] for key in ("rvv_lanes", "npu_macs", "dram_bandwidth_gbps", "npu_arithmetic_energy_scale", "mean_latency_us", "energy_per_frame_uj")))
