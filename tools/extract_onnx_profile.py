#!/usr/bin/env python3
"""Emit a deterministic AxisSim workload-profile.v1 JSON artifact from ONNX.

This is an offline developer tool. AxisSim's C++ CLI never imports ONNX or
ONNX Runtime; it consumes only the reviewed profile artifact. Static input
shapes are mandatory because unresolved ONNX dimensions cannot support a
defensible operation/traffic characterization.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from collections.abc import Iterable


SUPPORTED_NPU = {"Conv", "Gemm", "MatMul"}
SUPPORTED_RVV = {"Add", "Mul", "Sub", "Div", "Relu", "Clip", "Resize", "Sigmoid", "Tanh", "MaxPool", "AveragePool", "GlobalAveragePool", "BatchNormalization", "Flatten", "Reshape", "Softmax", "Identity"}
ELEMENT_BYTES = {1: 4, 2: 1, 3: 1, 4: 2, 5: 2, 6: 4, 7: 8, 9: 1, 10: 2, 11: 8, 12: 4, 13: 8, 14: 8, 15: 16, 16: 2}


def fail(message: str) -> None:
    raise ValueError(message)


def tensor_shapes(model) -> dict[str, tuple[list[int], int]]:
    """Return tensor name -> (fully static shape, ONNX element type)."""
    import onnx  # Optional dependency deliberately isolated to this tool.

    inferred = onnx.shape_inference.infer_shapes(model)
    result: dict[str, tuple[list[int], int]] = {}
    values: Iterable = list(inferred.graph.input) + list(inferred.graph.value_info) + list(inferred.graph.output)
    for value in values:
        tensor_type = value.type.tensor_type
        if not tensor_type.HasField("shape"):
            fail(f"Tensor {value.name!r} has no static shape")
        dimensions: list[int] = []
        for dimension in tensor_type.shape.dim:
            if not dimension.HasField("dim_value") or dimension.dim_value <= 0:
                fail(f"Tensor {value.name!r} has unresolved or non-positive dimension")
            dimensions.append(int(dimension.dim_value))
        if tensor_type.elem_type not in ELEMENT_BYTES:
            fail(f"Tensor {value.name!r} has unsupported element type {tensor_type.elem_type}")
        result[value.name] = (dimensions, int(tensor_type.elem_type))
    for initializer in inferred.graph.initializer:
        if initializer.data_type not in ELEMENT_BYTES:
            fail(f"Initializer {initializer.name!r} has unsupported element type {initializer.data_type}")
        result[initializer.name] = ([int(dimension) for dimension in initializer.dims], int(initializer.data_type))
    return result


def tensor_bytes(shapes: dict[str, tuple[list[int], int]], name: str) -> int:
    if name not in shapes:
        fail(f"Tensor {name!r} lacks inferred static shape/type information")
    dimensions, element_type = shapes[name]
    count = 1
    for dimension in dimensions:
        count *= dimension
    return count * ELEMENT_BYTES[element_type]


def product(values: Iterable[int]) -> int:
    result = 1
    for value in values:
        result *= value
    return result


def attribute_ints(node, name: str, fallback: list[int]) -> list[int]:
    for attribute in node.attribute:
        if attribute.name == name:
            return [int(value) for value in attribute.ints]
    return fallback


def conv_macs(node, shapes: dict[str, tuple[list[int], int]]) -> int:
    input_shape = shapes[node.input[0]][0]
    weight_shape = shapes[node.input[1]][0]
    output_shape = shapes[node.output[0]][0]
    if len(input_shape) != 4 or len(weight_shape) != 4 or len(output_shape) != 4:
        fail(f"Conv node {node.name or node.output[0]!r} must use rank-4 NCHW tensors")
    groups = 1
    for attribute in node.attribute:
        if attribute.name == "group":
            groups = int(attribute.i)
    if groups <= 0 or input_shape[1] != weight_shape[1] * groups:
        fail(f"Conv node {node.name or node.output[0]!r} has unsupported group/channel geometry")
    return product(output_shape) * weight_shape[1] * weight_shape[2] * weight_shape[3]


def gemm_macs(node, shapes: dict[str, tuple[list[int], int]]) -> int:
    output_shape = shapes[node.output[0]][0]
    if len(output_shape) < 2:
        fail(f"{node.op_type} node {node.name or node.output[0]!r} requires rank >= 2 output")
    # For an inferred output [..., M, N], use the reduction K from B when it
    # is statically known. This deliberately rejects ambiguous geometry.
    right_shape = shapes[node.input[1]][0]
    if len(right_shape) < 2:
        fail(f"{node.op_type} node {node.name or node.output[0]!r} requires rank >= 2 right operand")
    return product(output_shape) * right_shape[-2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=pathlib.Path, help="static-shape ONNX model")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="AxisSim workload-profile.v1 JSON path")
    parser.add_argument("--model-name", required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--license", required=True)
    parser.add_argument("--static-input", action="append", default=[], metavar="NAME=1x3x224x224",
                        help="required documentation of each static model input")
    arguments = parser.parse_args()
    try:
        import onnx
    except ImportError:
        print("error: install the optional extractor dependency with: python3 -m pip install 'onnx>=1.16,<2'", file=sys.stderr)
        return 2
    if not arguments.model.is_file():
        print(f"error: model does not exist: {arguments.model}", file=sys.stderr)
        return 2
    if not arguments.static_input:
        print("error: at least one --static-input declaration is required", file=sys.stderr)
        return 2
    try:
        model = onnx.load_model(arguments.model, load_external_data=False)
        declared_inputs = {}
        for declaration in arguments.static_input:
            name, separator, dimensions = declaration.partition("=")
            if not separator or not name or not dimensions:
                fail(f"Invalid --static-input declaration: {declaration!r}")
            declared_inputs[name] = [int(value) for value in dimensions.split("x")]
        graph_inputs = {value.name: value for value in model.graph.input}
        for name, dimensions in declared_inputs.items():
            if name not in graph_inputs:
                fail(f"Declared static input {name!r} does not exist in the model")
            shape = graph_inputs[name].type.tensor_type.shape.dim
            if len(shape) != len(dimensions):
                fail(f"Declared static input {name!r} has wrong rank")
            for dimension, value in zip(shape, dimensions):
                if value <= 0:
                    fail(f"Declared static input {name!r} has non-positive dimension")
                dimension.dim_value = value
        shapes = tensor_shapes(model)
        initializers = {initializer.name for initializer in model.graph.initializer}
        producer: dict[str, str] = {}
        operators: list[dict] = []
        for index, node in enumerate(model.graph.node):
            identifier = node.name or f"{node.op_type}_{index}"
            if node.op_type not in SUPPORTED_NPU | SUPPORTED_RVV:
                fail(f"Unsupported operator {node.op_type!r} at {identifier!r}; profile extraction stops rather than assigning zero cost")
            if not node.output:
                fail(f"Operator {identifier!r} has no output")
            output_name = node.output[0]
            activation_reads = sum(tensor_bytes(shapes, name) for name in node.input if name and name not in initializers)
            weight_bytes = sum(tensor_bytes(shapes, name) for name in node.input if name in initializers)
            activation_writes = sum(tensor_bytes(shapes, name) for name in node.output if name)
            dependencies = sorted({producer[name] for name in node.input if name in producer})
            entry = {
                "id": identifier,
                "op_type": node.op_type,
                "inputs": [name for name in node.input if name],
                "outputs": [name for name in node.output if name],
                "dependencies": dependencies,
                "activation_read_bytes": activation_reads,
                "activation_write_bytes": activation_writes,
                "weight_bytes": weight_bytes,
                "mapping_rule_version": "axissim.mapping.v1",
            }
            if node.op_type in SUPPORTED_NPU:
                entry.update({"mapping_class": "npu_compute", "macs": conv_macs(node, shapes) if node.op_type == "Conv" else gemm_macs(node, shapes),
                              "vector_elements": 0, "ops_per_element": 0})
            else:
                output_shape = shapes[output_name][0]
                entry.update({"mapping_class": "rvv_elementwise", "macs": 0, "vector_elements": product(output_shape), "ops_per_element": 1})
            operators.append(entry)
            for output in node.output:
                if output:
                    if output in producer:
                        fail(f"Tensor {output!r} has multiple producers")
                    producer[output] = identifier
        for name, dimensions in declared_inputs.items():
            if name not in shapes or shapes[name][0] != dimensions:
                fail(f"Declared static input {name!r} does not match inferred ONNX shape")
        artifact = {
            "schema_version": "axissim.workload-profile.v1",
            "source": {
                "model_name": arguments.model_name,
                "source_url": arguments.source_url,
                "license": arguments.license,
                "model_sha256": hashlib.sha256(arguments.model.read_bytes()).hexdigest(),
                "opset": max((entry.version for entry in model.opset_import), default=0),
                "extractor_version": "1",
                "static_input_shapes": declared_inputs,
            },
            "operators": operators,
            "scope": "Static ONNX operation and tensor-traffic characterization; not a measured runtime, compiler mapping, or silicon-power validation.",
        }
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (ValueError, onnx.checker.ValidationError, onnx.shape_inference.InferenceError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
