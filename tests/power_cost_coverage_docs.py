#!/usr/bin/env python3
"""Check the human power-cost inventory against the accepted manifest and C++ contract."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    manifest = json.loads((root / "experiments/budget_constrained_study/manifest.json").read_text())
    document = (root / "docs/power-cost-coverage.md").read_text()
    coverage_source = (root / "src/study_contract.cpp").read_text()
    simulation_source = (root / "src/simulation.hpp").read_text()
    metrics_source = (root / "src/metrics.cpp").read_text()
    config_source = (root / "src/config.hpp").read_text()
    budget_config = (root / "configs/power_budget/manifest.conf").read_text()

    try:
        factor_section = document.split("### Candidate factor to machine-readable coverage map", 1)[1]
    except IndexError:
        print("power-cost coverage contract table is missing", file=sys.stderr)
        return 1
    factor_section = factor_section.split("\nApplicability follows", 1)[0]

    factor_rows: dict[str, list[str]] = {}
    for line in factor_section.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if len(cells) < 4:
            continue
        for factor_id in re.findall(r"`([a-z][a-z0-9_]*)`", cells[0]):
            factor_rows[factor_id] = cells

    expected_factors = set(manifest["factor_order"])
    documented_factors = set(factor_rows)
    missing_factors = sorted(expected_factors - documented_factors)

    cost_terms = set(re.findall(r'cost_term\(terms,\s*"([^"]+)"', coverage_source))
    omitted_terms = set(re.findall(r'missing_term\(terms,\s*"([^"]+)"', coverage_source))
    emitted_terms = cost_terms | omitted_terms
    missing_terms = sorted(term for term in emitted_terms if f"`{term}`" not in document)

    mapping_drift = []
    coverage_body = coverage_source.split("build_power_coverage", 1)[1]

    def branch_for(factor_id: str) -> str:
        marker = f'factor.id == "{factor_id}"'
        start = coverage_body.find(marker)
        if start < 0:
            raise ValueError(f"factor not found in C++ coverage function: {factor_id}")
        opening = coverage_body.find("{", start)
        if opening < 0:
            raise ValueError(f"coverage branch has no body: {factor_id}")
        depth = 0
        for index in range(opening, len(coverage_body)):
            if coverage_body[index] == "{":
                depth += 1
            elif coverage_body[index] == "}":
                depth -= 1
                if depth == 0:
                    return coverage_body[opening:index + 1]
        raise ValueError(f"unterminated coverage branch: {factor_id}")

    expected_applicability = {
        "npu_macs": "applicable = has_npu;",
        "rvv_lanes": "applicable = has_vector;",
        "dram_bandwidth_gbps": "applicable = has_memory;",
        "sram_capacity_bytes": "applicable = physical;",
        "weight_sram_capacity_bytes": "applicable = physical || has_profile_residency;",
        "npu_weights_resident": "applicable = physical || has_profile_residency;",
        "npu_tile_m": "applicable = physical;",
        "npu_tile_n": "applicable = physical;",
        "npu_tile_k": "applicable = physical;",
        "scheduler_policy": 'applicable = factor.id == "scheduler_policy" || physical;',
        "rvv_chunk_elements": 'applicable = factor.id == "scheduler_policy" || physical;',
        "rvv_frequency_ghz": 'applicable = factor.id == "rvv_frequency_ghz" ? has_vector : has_npu;',
        "npu_frequency_ghz": 'applicable = factor.id == "rvv_frequency_ghz" ? has_vector : has_npu;',
        "npu_arithmetic_energy_scale": "applicable = has_npu;",
        "arithmetic_voltage_ratio": "applicable = has_npu;",
        "control_period_us": "applicable = config.control_period_us > 0.0;",
        "control_deadline_us": "applicable = config.control_period_us > 0.0;",
        "release_schedule": "hardware_cost_factor = false;",
    }
    for factor_id, cells in factor_rows.items():
        try:
            branch = branch_for(factor_id)
        except ValueError as exc:
            mapping_drift.append(str(exc))
            continue
        source_represented = set(re.findall(r'cost_term\(terms,\s*"([^"]+)"', branch))
        source_omitted = set(re.findall(r'missing_term\(terms,\s*"([^"]+)"', branch))
        doc_represented = {
            term for term in re.findall(r"`([^`]+)`", cells[2])
            if term.startswith(("static.", "dynamic."))
        }
        doc_omitted = {
            term for term in re.findall(r"`([^`]+)`", cells[3]) if term.startswith("missing.")
        }
        if source_represented != doc_represented:
            mapping_drift.append(f"{factor_id}: represented terms source={sorted(source_represented)} doc={sorted(doc_represented)}")
        if source_omitted != doc_omitted:
            mapping_drift.append(f"{factor_id}: omitted terms source={sorted(source_omitted)} doc={sorted(doc_omitted)}")
        expected_condition = expected_applicability.get(factor_id)
        if expected_condition and expected_condition not in branch:
            mapping_drift.append(f"{factor_id}: expected applicability condition absent from C++ branch")

    if "(coefficient > 0.0 ? terms.represented : terms.omitted)" not in coverage_source:
        mapping_drift.append("C++ zero-coefficient coverage rule changed")
    status_rule = '!applicable || !hardware_cost_factor ? "n/a" : terms.omitted.empty() ? "complete"'
    if status_rule not in coverage_source or 'terms.represented.empty() ? "missing" : "partial"' not in coverage_source:
        mapping_drift.append("C++ n/a/complete/missing/partial status rule changed")

    source_equations = [
        (simulation_source, r"static_cast<double>\(elements\)\s*\*\s*c_\.preprocess_ops_per_element\s*\*\s*c_\.vector_op_pj", "vector pJ/op equation"),
        (simulation_source, r"macs\s*\*\s*c_\.npu_mac_pj\s*\*\s*c_\.npu_arithmetic_energy_scale\s*\*\s*c_\.arithmetic_voltage_ratio\s*\*\s*c_\.arithmetic_voltage_ratio", "NPU MAC scale and squared voltage-ratio equation"),
        (simulation_source, r"static_cast<double>\(task\.transfer_bytes\)\s*\*\s*c_\.dram_byte_pj", "DRAM byte equation"),
        (simulation_source, r"static_cast<double>\(task\.transfer_bytes\)\s*\*\s*c_\.dma_byte_pj", "DMA byte equation"),
        (metrics_source, r"config\.static_power_w\s*\+\s*static_cast<double>\(config\.npu_macs\)\s*\*\s*config\.npu_static_power_per_mac_w", "static-power equation"),
        (metrics_source, r"accounting\.it_energy_pj\s*/\s*makespan_us\s*/\s*1'000'000\.0", "average IT power conversion"),
    ]
    for source, pattern, label in source_equations:
        if not re.search(pattern, source):
            mapping_drift.append(f"source equation changed; review inventory: {label}")
    for formula in (
        "E_vector = modeled_vector_operations × vector_op_pj",
        "E_NPU    = modeled_MACs × npu_mac_pj × arithmetic_scale × profile_scale × voltage_ratio²",
        "E_DRAM   = transferred_bytes × dram_byte_pj",
        "E_DMA    = transferred_bytes × dma_byte_pj",
        "P_IT_avg    = E_IT_pJ / interval_us / 1,000,000",
    ):
        if formula not in document:
            mapping_drift.append(f"documented equation missing or changed: {formula}")

    config_defaults = {
        "vector_op_pj": "0.40",
        "npu_mac_pj": "0.20",
        "npu_arithmetic_energy_scale": "1.0",
        "arithmetic_voltage_ratio": "1.0",
        "dram_byte_pj": "15.0",
        "dma_byte_pj": "1.5",
        "static_power_w": "0.0",
        "npu_static_power_per_mac_w": "0.0",
        "rvv_static_power_per_lane_w": "0.0",
        "sram_static_power_per_kib_w": "0.0",
        "dram_interface_static_power_w": "0.0",
        "pue": "1.0",
        "deployment_xpu_count": "1",
        "deployment_hours": "0.0",
        "electricity_cost_per_kwh": "0.0",
    }
    missing_defaults = []
    for field, value in config_defaults.items():
        line = next((row for row in document.splitlines() if f"`{field}`" in row), "")
        pattern = rf"\b(?:double|int|std::uint64_t)\s+{re.escape(field)}\s*=\s*([^;]+);"
        match = re.search(pattern, config_source)
        source_value = match.group(1).strip() if match else None
        if not line or source_value is None or source_value not in line or value != source_value:
            missing_defaults.append(f"{field}={value}")

    budget_values = {
        "static_power_w": "0.05",
        "npu_static_power_per_mac_w": "0.00003",
        "rvv_static_power_per_lane_w": "0.002",
        "sram_static_power_per_kib_w": "0.0001",
        "dram_interface_static_power_w": "0.02",
        "it_power_budget_w": "0.23",
    }
    missing_budget_values = []
    for field, expected in budget_values.items():
        match = re.search(rf"(?m)^\s*{re.escape(field)}\s*=\s*([^#\r\n]+)", budget_config)
        value = match.group(1).strip() if match else None
        if value != expected:
            missing_budget_values.append(f"{field}: expected config {expected}, found {value}")
            continue
        if value not in document:
            missing_budget_values.append(f"{field}={value}")

    if missing_factors or missing_terms or mapping_drift or missing_defaults or missing_budget_values:
        if missing_factors:
            print(f"manifest factors absent from coverage table: {missing_factors}", file=sys.stderr)
        if missing_terms:
            print(f"C++ coverage terms absent from documentation: {missing_terms}", file=sys.stderr)
        if mapping_drift:
            print(f"coverage mapping or equation drift: {mapping_drift}", file=sys.stderr)
        if missing_defaults:
            print(f"Config defaults absent from documentation rows: {missing_defaults}", file=sys.stderr)
        if missing_budget_values:
            print(f"reference budget values absent from documentation: {missing_budget_values}", file=sys.stderr)
        return 1

    print(f"Power-cost coverage documentation matches {len(expected_factors)} manifest factors, "
          f"{len(emitted_terms)} C++ coverage terms and their factor mappings, the implemented status/equation rules, "
          f"{len(config_defaults)} defaults, and "
          f"{len(budget_values)} reference-budget values.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
