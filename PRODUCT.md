# Product

<!-- impeccable:product-schema 1 -->

## Platform

adaptive

## Users

The primary user is a computer-architecture or performance-modeling engineer exploring a hypothetical AI SoC before silicon exists. A secondary user is an interviewer or technical reviewer who needs to understand the model, its evidence, and its limits quickly.

## Product Purpose

AxisSim is a native desktop architecture workbench for testing how compute resources, vector execution, memory bandwidth, scheduling, and energy assumptions affect end-to-end Physical AI workloads. Success means turning a configuration change into a reproducible result, an identified bottleneck, and an appropriately bounded engineering recommendation.

## Positioning

AxisSim connects an interactive architecture-exploration workflow to a deterministic C++ simulator, explicit assumption files, public Ara RTL reference points, and machine-readable outputs. It is not a chip-power calculator, a functional instruction-set simulator, or a physical-design signoff tool.

## Operating Context

Users select or edit a scenario manifest, apply temporary architectural overrides, run the model, inspect latency, energy, traffic, deadlines, timelines, and implementation-sensitivity proxies, then export evidence or run controlled sweeps. The app is used on a desktop during engineering analysis, portfolio demonstrations, and technical interviews.

## Capabilities and Constraints

- Native Qt 6 Widgets desktop application backed by a C++20 command-line simulator.
- Models a hypothetical Physical AI pipeline with DMA/shared DRAM, RVV-style preprocessing and postprocessing, tiled GEMM acceleration, scheduling, and explicit energy accounting.
- Provides bounded area, thermal, reliability, facility-power, and deployment-cost sensitivity proxies.
- Validates a vector compute envelope against public Ara RTL results; this does not validate silicon power, caches, a complete SoC, or physical implementation.
- Arithmetic-efficiency inputs are hypothetical iso-performance scenarios. AxisSim does not model Titan Core, Velaura silicon, low-voltage cells, timing closure, yield, or reliability signoff.
- Existing simulator behavior, reproducible CLI workflows, and honest scope labels must remain intact through UI changes.

## Brand Commitments

The product name is AxisSim. Its voice is precise, direct, engineering-oriented, and explicit about whether a number is measured, reproduced, modeled, or illustrative. The interface must feel like a serious analysis tool without using technical ornament as a substitute for clarity.

## Evidence on Hand

- Reproducible baseline and design-space studies in `experiments/` and `docs/results.md`.
- Public/reproduced Ara reference data, raw logs, provenance, and validation discussion in `reference_data/ara/` and `docs/ara-validation.md`.
- Automated model and GUI smoke tests in `CMakeLists.txt` and `tests/`.
- Existing packaged macOS application, icon assets, charts, JSON export, and CLI output.
- No measured AxisSim silicon-power data or proprietary Velaura implementation data exists; future surfaces must not imply otherwise.

## Product Principles

1. Show the causal chain from assumption to modeled result to architectural decision.
2. Make model scope and evidence quality visible at the point of interpretation.
3. Favor fast, repeatable comparisons over decorative dashboards.
4. Preserve expert depth while giving first-time users an obvious successful path.
5. Never present sensitivity proxies or modeled estimates as measured silicon truth.

## Accessibility & Inclusion

The desktop workflow should be usable with keyboard navigation, expose meaningful accessible names and roles, maintain WCAG AA-equivalent contrast, preserve legibility under larger system text, and avoid relying on color alone to communicate status.
