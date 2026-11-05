# AxisSim architecture-workbench redesign audit

Status: Phase 1 complete; Phase 2 design system awaiting approval before the visual v0.

Branch: `ui/architecture-workbench`

## Decision record

- **Mode:** Redesign · Overhaul.
- **Preserve:** C++/Qt 6 production architecture, CLI commands and flags, layered configuration semantics, numerical results, JSON schema, trace CSV, calibration evidence, exports, keyboard accessibility, theme preference, and test coverage.
- **Improve:** task hierarchy, first-run usefulness, scenario configuration, pipeline interpretation, sweep exploration, candidate comparison, evidence visibility, responsive layouts, and visual identity.
- **Remove:** the raw manifest as the dominant default interaction, repeated outlined KPI cards, nested panel chrome, redundant tabs/navigation, decorative status badges, and tables that repeat conclusions already visible elsewhere.
- **Protected contracts:** `run`, `validate`, `sweep`, `calibrate`, and `analyze`; `--set`, `--json`, `--trace`, `--csv`, `--reference`, and `--ranges`; configuration keys and include behavior; versioned result schema; trace columns; public Ara evidence; existing model boundaries and caveats.
- **Highest-risk change:** a guided editor can accidentally diverge from the layered config parser. The safe design treats guided fields as explicit run overrides and retains a layer-aware advanced manifest view. It does not silently rewrite included files.
- **Rollback:** no production redesign begins until the visual v0 is approved. Work remains isolated on the dedicated branch.

## Current product map

### Simulation and configuration

- `src/main.cpp` owns config parsing, validation, the event scheduler, timing and energy accounting, CLI commands, trace CSV, sweep CSV, calibration CSV, and result JSON.
- `src/config.hpp` exposes roughly 70 explicit inputs spanning workload, RVV, NPU, memory, scheduling, energy, power, deployment, area, thermal, and reliability assumptions.
- Scenarios compose `workload.conf`, `architecture.conf`, `implementation.conf`, and `energy.conf` through a manifest. Duplicate and unknown keys are rejected.
- Validation already covers invalid RVV geometry, invalid scheduler/memory modes, incomplete control workloads, insufficient tile SRAM, insufficient resident-weight SRAM, boolean fields, energy constraints, power/deployment constraints, and proxy constraints.

### Desktop integration

- `src/gui/main.cpp` launches the same CLI through `QProcess`; there is no second simulation model.
- A run produces temporary JSON and trace artifacts, then populates the interface.
- The current guided controls expose RVV lanes, NPU MACs, DRAM bandwidth, and NPU arithmetic-energy scale as run-only overrides.
- The raw manifest editor supports open, edit, save-as, and dirty-state protection.
- Lane sweep runs the generic CLI sweep command for 2, 4, 8, 16, and 32 RVV lanes.
- Ara2 check runs the public calibration path and displays its scope boundary.
- JSON export preserves the exact CLI-produced result artifact.

### Existing result surfaces

- Four headline measurements: mean latency, IT energy/frame, deadline misses, and total DRAM traffic.
- Energy composition bar.
- Detailed metrics table.
- Frame-zero trace table.
- RVV lane sweep chart and table.
- Area, thermal, and reliability proxy table.
- Complete CLI log and validation notes.

### Real data available for stronger visualizations

| Product question | Existing source | Safe derived view |
|---|---|---|
| What limits performance? | Trace task/resource/start/end fields | Resource-lane pipeline, stage share, serialized wait/compute path |
| What changes with RVV lanes? | Sweep CSV | Directly labeled latency curve and marginal speedup |
| Where is energy consumed? | Result JSON energy ledger | Directly labeled part-to-whole bar and arithmetic-vs-data-movement grouping |
| Which configuration is better? | Multiple JSON results or sweep rows | Baseline-relative deltas and latency/energy Pareto plot |
| Are constraints met? | Deadlines, power, area, thermal result fields | Compact constraint ledger with pass/fail reasoning |
| How reliable is the conclusion? | Scenario/model/schema, assumptions, calibration CSV, docs | Evidence panel separating reproduced, published, assumed, and disabled inputs |

No new benchmark values are needed for the redesign. The checked-in baseline run provides a real pipeline of 861.060 µs, including 460.800 µs RVV preprocessing, 120.965 µs NPU operand transfer, and 32.768 µs NPU compute. The checked-in energy ledger reports 107.758 µJ/frame, of which DRAM is 90.747 µJ. The RVV sweep reports 2,010.361 µs at two lanes and 573.735 µs at 32 lanes.

## Current experience audit

### 1. The interface starts with controls instead of an architectural question

The top of the window contains scenario selection, four override dropdowns, four secondary actions, and status chrome before the user reaches a result. This presents implementation controls before explaining the workload or the decision being evaluated.

**Cause in code:** `buildInterface()` adds a full-width header and a second full-width command panel above the result workspace.

**Correction:** one compact command bar contains scenario, run, and save state. Experiment creation belongs in Experiment Lab, not above every result view.

### 2. The raw manifest competes with the analysis

The editor permanently occupies roughly one third of the main workspace even after a successful run. Most users need five to twelve high-leverage fields, not seventy raw keys at once.

**Cause in code:** the editor and content panel are permanent siblings in the primary horizontal splitter.

**Correction:** Overview devotes the canvas to architectural interpretation. Experiment Lab uses structured parameter groups; the raw manifest becomes an advanced, layer-aware drawer.

### 3. The pipeline is hidden in a table

The trace contains the strongest explanation of causality, but Timeline exposes it as rows. A user must mentally reconstruct resource occupancy and stage order.

**Cause in code:** `populateTrace()` copies five CSV columns into `QTableWidget`; there is no resource-lane visualization.

**Correction:** a dominant aligned-lane timeline shows DMA/DRAM, RVV, NPU, and control on one time axis with direct stage labels and a deadline marker.

### 4. Measurements are repetitive rather than interpretive

Four equal KPI boxes give traffic and deadline outcome the same visual weight as latency and energy, then the table repeats many values. The interface reports facts but does not state the bottleneck or highest-leverage change.

**Cause in code:** `KpiCard` is instantiated four times with identical geometry, while the detailed table is the primary explanatory surface.

**Correction:** use an inline measurement rail with two primary values and subordinate constraint/delta values. Add a model-backed finding such as “RVV preprocessing is 53.5% of frame latency” or “DRAM is 84.2% of modeled dynamic energy.”

### 5. Navigation and result tabs duplicate each other

The left navigation selects result tabs while the same tab bar remains visible inside the result panel. “Experiment Lab” only focuses the raw text editor rather than opening a distinct workflow.

**Cause in code:** nav IDs map directly to `QTabWidget` indices; one nav item only changes keyboard focus.

**Correction:** use four product-level workspaces—Overview, Experiment Lab, Compare, Evidence. Local tabs appear only where they represent views inside one workspace.

### 6. Outlines flatten the hierarchy

The latest theme added complete borders to nearly every panel, metric, field, tab, table, and header. This prevents regions from blending, but makes every rectangle compete equally.

**Cause in code:** broad QSS selectors give strong borders to `commandPanel`, `editorPanel`, `contentPanel`, `metricStrip`, every `metricPanel`, fields, tabs, tables, header, and navigation.

**Correction:** rely on canvas contrast, spacing, and a small number of structural rules. Use a strong outline only for focus, selection, errors, and the active experiment boundary.

### 7. The visual identity is attached to chrome rather than purpose

The dark navy rail/header, teal selection, serif UI type, rounded boxes, and repeated metrics can describe many generic dashboards. They do not specifically express scheduling, pipelines, resource contention, or evidence quality.

**Correction:** the architecture trace becomes the signature visual. Resource lanes, measurement ticks, comparison deltas, and provenance marks form AxisSim’s identity.

## Improved workflows

### 1. Open and configure a scenario

1. Open or select a scenario from the command bar.
2. See workload, architecture, model version, and evidence status immediately.
3. Enter Experiment Lab for grouped controls: workload, RVV, NPU/memory, scheduling, and energy/power.
4. Changed fields appear in a compact “Experiment delta” ledger with units and baseline values.
5. Validate inline. Cross-field constraints explain required SRAM or missing control parameters before run.
6. Run as a temporary experiment, save a scenario copy, or discard changes.

### 2. Run a baseline simulation

1. “Run baseline” is the single primary command.
2. The run state occupies the pipeline region without shifting the layout.
3. On success, Overview updates the pipeline, measurements, bottleneck finding, and evidence timestamp together.
4. On failure, the invalid field group and exact CLI diagnostic remain visible; no generic modal is required for expected validation errors.

### 3. Interpret timing and utilization

1. Read the frame latency and deadline margin from the measurement rail.
2. Follow the resource-lane pipeline from ingest through postprocess.
3. Select a stage to see duration, resource, share of latency, and relevant assumption.
4. Switch between first frame and steady-state frames when weight residency changes traffic.

### 4. Explore architecture changes

1. Choose a parameter and sweep values in Experiment Lab.
2. See a directly labeled curve with deadline threshold and marginal improvement.
3. Promote any sweep point to a named candidate without losing the baseline.

### 5. Compare candidate configurations

1. Compare two to four saved run artifacts.
2. Read baseline-relative deltas for latency, energy, traffic, and constraints.
3. Inspect a latency/energy tradeoff plot and a changed-parameters ledger.
4. Identify dominated candidates and the reason a design is infeasible.

### 6. Export and inspect evidence

1. Export the selected run JSON, trace CSV, or comparison CSV from one export menu.
2. Evidence shows scenario/model/schema, explicit assumptions, disabled proxies, calibration source/status, and known limitations.
3. Published and reproduced data are visually distinct from configured assumptions; nothing is labeled “validated” without scope.

## Proposed information architecture

### Persistent application shell

- Compact top command bar: scenario, dirty state, run, stop, export, command search, appearance.
- Narrow navigation rail: Overview, Experiment Lab, Compare, Evidence.
- Persistent status strip: model version, result schema, run timestamp, and modeled-estimate boundary.

### Overview

- Measurement rail: mean/P99 latency, deadline margin, energy/frame, and dominant energy source.
- Dominant pipeline: aligned DMA/DRAM, RVV, NPU, and control lanes using real trace events.
- Finding margin: two direct, model-backed observations and the assumptions that make them true.
- Energy ledger: directly labeled part-to-whole bar, not a legend-heavy card.
- Resource and constraint summary: utilization derived from trace, power/area/thermal status only when configured.

### Experiment Lab

- Parameter navigator grouped by workload, RVV, NPU/memory, scheduling, and energy/power.
- Structured editor with unit-aware numeric fields, constrained choices, inline help, and cross-field validation.
- Experiment delta ledger comparing edited values with the loaded scenario.
- Actions: run experiment, save scenario copy, reset group, open advanced manifest.
- Advanced manifest drawer preserves comments, includes, and expert access without dominating the default view.

### Compare

- Candidate list with baseline pinning and run provenance.
- Baseline-relative delta table.
- Latency-versus-energy plot with feasibility encoding and direct labels.
- Parameter-difference ledger and bottleneck change explanation.

### Evidence

- Run provenance and exported artifact paths.
- Assumption registry grouped as workload facts, architecture choices, calibrated inputs, and illustrative proxies.
- Calibration table with published/reproduced status, source revision, error, and scope boundary.
- Model limitations and disabled proxy states.
- Complete CLI log as a secondary disclosure.

## Component plan

- `WorkbenchShell`: command bar, navigation, workspace switching, status strip.
- `ScenarioSelector`: recent scenarios, presets, dirty state, open/save-copy behavior.
- `RunController`: idle/running/success/validation-error/process-error states.
- `MeasurementRail`: aligned values with units, delta, and constraint context; no repeated cards.
- `ResourceTimeline`: actual trace events on shared time scale, deadline marker, selection details.
- `EnergyLedger`: directly labeled horizontal allocation bar and arithmetic/data-movement grouping.
- `FindingMargin`: evidence-linked conclusion text generated only from deterministic result/trace comparisons.
- `ParameterNavigator` and `ParameterEditor`: metadata-driven fields with units, ranges, help, and validation.
- `ExperimentDelta`: baseline/current values and reset actions.
- `SweepPlot`: parameter scale, latency/energy series, deadline threshold, promotable points.
- `CandidateComparison`: result-artifact registry, deltas, feasibility, and parameter differences.
- `EvidenceLedger`: provenance, assumption type, calibration status, and limitations.
- `AdvancedManifestDrawer`: current raw editor behavior behind explicit progressive disclosure.

## Design read and calibrated dials

```yaml
Design Read:
  artifact: native desktop architecture-analysis workstation
  audience: hardware architects, performance engineers, researchers, and architecture students
  visual-language: precision engineering workbench; restrained tool chrome with publication-grade technical figures
  mode: redesign-overhaul
  constraints: Qt 6 Widgets, real simulator outputs only, keyboard accessibility, 1100–1920 px desktop widths, high DPI, light/dark modes
  visual-variance: 5
  motion-intensity: 2
  information-density: 8
  asset-dependence: 1
  brand-fidelity: 5
```

Concrete consequences:

- Variance 5: stable workstation grid with one distinctive architectural layer—the resource timeline and evidence margin.
- Motion 2: only run progress, selection transitions, and short focus feedback; no ambient animation.
- Density 8: multiple useful views per screen, but progressive disclosure protects first-time comprehension.
- Asset dependence 1: the product is carried by real data, typography, and interaction rather than imagery.
- Fidelity 5: retain the AxisSim name, icon, model voice, and teal lineage only where useful; replace the current dashboard vocabulary.

## Positioning questions

### Overview

- **Narrative role:** answer “what happened, why, and what should I inspect next?”
- **Viewing distance:** laptop/desktop at normal engineering-workstation distance.
- **Temperature:** authoritative, analytical, calm.
- **Capacity:** pipeline occupies the largest region; measurements and findings support it rather than compete.

### Experiment Lab

- **Narrative role:** turn model configuration into a safe, explainable experiment.
- **Viewing distance:** close interactive work with keyboard and mouse.
- **Temperature:** precise and instructional, never wizard-like or playful.
- **Capacity:** one parameter group and its consequences visible at once; advanced detail remains one action away.

## Proposed design system for approval

- **Anchor:** custom AxisSim system. Linear contributes restrained interaction and keyboard ergonomics; Tufte contributes direct labels and data-ink; Bloomberg contributes scan-friendly density. Their palettes and visual skins are not mixed.
- **Light palette:** mineral paper `#F2F1EC`, work surface `#FAFAF7`, graphite ink `#202526`, secondary ink `#626A6B`, rules `#C9CDCA`, strong rule `#858D8D`, signal vermilion `#C45132`.
- **Dark palette:** warm graphite `#171918`, work surface `#202321`, primary ink `#F1F0EA`, secondary ink `#AEB4AF`, rules `#3E4441`, signal vermilion `#E0704E`.
- **Resource colors:** a restrained, accessible categorical set reserved for real trace/energy categories; every mark is also directly labeled.
- **Typography:** IBM Plex Sans for interface and explanation; IBM Plex Mono for measurements, axes, parameters, and logs. This proposal intentionally replaces the current Times New Roman UI because the serif treatment weakens compact control scanning. It remains open for approval.
- **Spacing:** 4 / 8 / 12 / 16 / 24 / 32 / 48. Major workspaces use a 12-column grid; dense tables use 8 px row rhythm.
- **Radius:** 0 for plots/tables/rules, 3 px for fields, 5 px for bounded interactive surfaces. No card stacks or large soft rectangles.
- **Elevation:** essentially flat. Canvas/surface contrast and structural rules define regions; one subtle shadow is reserved for transient menus and drawers.
- **Borders:** quiet structural separators only. Strong outlines are reserved for keyboard focus, selected candidates, invalid fields, and active drag targets.
- **Chart language:** direct labels, shared axes, sparse reference rules, deadline/power thresholds, no decorative charts, no redundant legend when labels fit.
- **Motion:** 100–160 ms state feedback; 220 ms drawer/workspace transition; reduced-motion mode removes spatial transitions.
- **Accessibility:** visible 2 px focus treatment, color never carries meaning alone, explicit units, tab order follows visual order, status changes are announced, and charts expose textual summaries.

## Visual v0 scope after approval

The checkpoint prototype will show two realistic views using only the checked-in baseline and configuration values:

1. **Architecture Overview:** command bar, measurement rail, real eight-stage resource timeline, directly labeled energy ledger, and evidence-linked bottleneck finding.
2. **Experiment Lab:** grouped RVV/NPU/memory controls, baseline-to-experiment delta ledger, inline SRAM/validation explanation, and an advanced-manifest disclosure.

The prototype will not modify production Qt code. After visual approval, the redesign will be implemented in reviewable milestones and each milestone will include running-app screenshots, tests, defects, and the next step.
