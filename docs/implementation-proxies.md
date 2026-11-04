# Implementation proxy models

AxisSim includes optional, assumption-driven proxies for early architecture exploration. They are disabled by default and are not physical-design signoff, thermal validation, or reliability prediction.

## Area

The area proxy sums explicit base, per-NPU-MAC, per-RVV-lane, and per-KiB SRAM assumptions. It can check a user-supplied area limit. It does not model placement, routing, process-node density, memories, clock trees, or physical-design closure.

## Thermal

The thermal proxy is a single-node first-order RC response driven by modeled average IT power. Users provide ambient temperature, thermal resistance, thermal capacitance, and an optional temperature limit. It does not model hotspots, packages, heatsinks, airflow, liquid cooling, throttling, or junction-temperature validation.

## Reliability

The reliability proxy extrapolates a user-supplied FIT rate and optional voltage-risk multiplier across deployment device-hours. It does not derive FIT from voltage, model soft-error mechanisms, ECC, aging, process variation, yield, or Velaura technology.

These proxies exist to make architecture constraints explicit and sensitivity-test decisions. Results must always be reported with their assumptions.
