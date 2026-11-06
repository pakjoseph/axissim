#!/usr/bin/env python3
"""Convert canonical MNIST IDX gzip files into AxisSim evaluator NPZ input."""
from __future__ import annotations
import argparse
import gzip
import struct
from pathlib import Path
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("images", type=Path)
parser.add_argument("labels", type=Path)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
with gzip.open(args.images, "rb") as source:
    magic, count, rows, columns = struct.unpack(">IIII", source.read(16))
    if magic != 2051: raise SystemExit("error: invalid IDX image magic")
    inputs = np.frombuffer(source.read(), dtype=np.uint8).reshape(count, 1, rows, columns).astype(np.float32) / 255.0
with gzip.open(args.labels, "rb") as source:
    magic, count = struct.unpack(">II", source.read(8))
    if magic != 2049: raise SystemExit("error: invalid IDX label magic")
    labels = np.frombuffer(source.read(), dtype=np.uint8)
if len(inputs) != len(labels): raise SystemExit("error: image/label count mismatch")
args.output.parent.mkdir(parents=True, exist_ok=True)
np.savez_compressed(args.output, inputs=inputs, labels=labels)
