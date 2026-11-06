#!/usr/bin/env python3
"""Create a controlled QDQ INT8 ONNX candidate from a declared NPZ subset."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import numpy as np
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("model", type=Path)
parser.add_argument("dataset", type=Path)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--metadata", required=True, type=Path)
parser.add_argument("--input-key", default="inputs")
parser.add_argument("--input-name", default="Input3")
parser.add_argument("--calibration-count", type=int, default=256)
args = parser.parse_args()
archive = np.load(args.dataset, allow_pickle=False)
inputs = archive[args.input_key]
if args.calibration_count < 1 or args.calibration_count > len(inputs): raise SystemExit("error: invalid calibration-count")
class Reader(CalibrationDataReader):
    def __init__(self): self.index = 0
    def get_next(self):
        if self.index >= args.calibration_count: return None
        value = {args.input_name: inputs[self.index:self.index + 1]}; self.index += 1; return value
args.output.parent.mkdir(parents=True, exist_ok=True)
quantize_static(args.model, args.output, Reader(), quant_format=QuantFormat.QDQ,
                activation_type=QuantType.QInt8, weight_type=QuantType.QInt8)
args.metadata.parent.mkdir(parents=True, exist_ok=True)
args.metadata.write_text(json.dumps({"method":"onnxruntime.quantize_static","format":"QDQ","activation_type":"QInt8","weight_type":"QInt8","calibration_count":args.calibration_count,"input_name":args.input_name,"input_key":args.input_key}, indent=2)+"\n")
