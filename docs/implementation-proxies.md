# Implementation proxy models

AxisSim includes optional, assumption-driven proxies for early architecture exploration. They are disabled by default and are not physical-design signoff, thermal validation, or reliability prediction.

## Area

The area proxy sums explicit base, per-NPU-MAC, per-RVV-lane, and per-KiB SRAM assumptions. It can check a user-supplied area limit. It does not model placement, routing, process-node density, memories, clock trees, or physical-design closure.

`area = base + MACs × area/MAC + RVV lanes × area/lane + SRAM KiB × area/KiB`

## Thermal

The thermal proxy is a single-node first-order RC response driven by modeled average IT power. Users provide ambient temperature, thermal resistance, thermal capacitance, and an optional temperature limit. It does not model hotspots, packages, heatsinks, airflow, liquid cooling, throttling, or junction-temperature validation.

`Tsteady = Tambient + PIT × Rtheta`

`T(t) = Tambient + (Tsteady - Tambient) × (1 - exp(-t / (Rtheta × Cthermal)))`

## Reliability

The reliability proxy extrapolates a user-supplied FIT rate and optional voltage-risk multiplier across deployment device-hours. It does not derive FIT from voltage, model soft-error mechanisms, ECC, aging, process variation, yield, or Velaura technology.

`expected failures = FIT × voltage-risk multiplier × device count × hours / 1e9`

These proxies exist to make architecture constraints explicit and sensitivity-test decisions. Results must always be reported with their assumptions.

Run the checked-in, deliberately illustrative scenario with:

```sh
./build/axissim run configs/implementation_proxy/manifest.conf \
  --json experiments/implementation_proxy_result.json
```

Every coefficient is visible in the manifest and machine-readable result. The scenario is a software-model demonstration, not characterization data.
