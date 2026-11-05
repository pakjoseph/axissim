---
version: alpha
name: AxisSim Precision Lab
description: A precise, calm, evidence-first design language for a native architecture exploration workbench.
colors:
  primary: "#C45132"
  chrome: "#151D26"
  canvas: "#F2F1EC"
  surface: "#FAFAF7"
  surface-muted: "#E7E5DF"
  surface-hover: "#EDECE6"
  text-primary: "#202526"
  text-secondary: "#5D6465"
  text-muted: "#747B79"
  text-inverse: "#FFFFFF"
  border: "#C9CDCA"
  border-strong: "#858D8D"
  accent: "#C45132"
  accent-hover: "#9A3D25"
  accent-soft: "#F2DFD7"
  success: "#237B5B"
  warning: "#9B6219"
  danger: "#B93F46"
  focus: "#147D85"
  chart-dma: "#3F6F77"
  chart-rvv: "#A94B31"
  chart-npu: "#465D83"
  chart-control: "#775B79"
  chart-background: "#DEDFD9"
  dark-canvas: "#171918"
  dark-surface: "#202321"
  dark-surface-muted: "#292D2B"
  dark-surface-hover: "#2B302D"
  dark-text-primary: "#F1F0EA"
  dark-text-secondary: "#AEB4AF"
  dark-text-muted: "#8A938E"
  dark-border: "#3E4441"
  dark-border-strong: "#535B56"
  dark-accent: "#E0704E"
  dark-accent-hover: "#EE8A6B"
  dark-accent-soft: "#382A25"
  dark-accent-text: "#F0A58D"
  dark-chart-rvv: "#E2A85B"
  dark-chart-npu: "#839BE2"
  dark-chart-control: "#C08AD1"
typography:
  page-title:
    fontFamily: IBM Plex Sans
    fontSize: 24px
    fontWeight: 600
    lineHeight: 1.25
  section-title:
    fontFamily: IBM Plex Sans
    fontSize: 18px
    fontWeight: 600
    lineHeight: 1.3
  card-title:
    fontFamily: IBM Plex Sans
    fontSize: 16px
    fontWeight: 600
    lineHeight: 1.4
  body:
    fontFamily: IBM Plex Sans
    fontSize: 14px
    fontWeight: 400
    lineHeight: 1.5
  body-strong:
    fontFamily: IBM Plex Sans
    fontSize: 14px
    fontWeight: 600
    lineHeight: 1.5
  label:
    fontFamily: IBM Plex Sans
    fontSize: 12px
    fontWeight: 500
    lineHeight: 1.4
  caption:
    fontFamily: Times New Roman
    fontSize: 12px
    fontWeight: 400
    lineHeight: 1.4
  metric:
    fontFamily: IBM Plex Mono
    fontSize: 26px
    fontWeight: 600
    lineHeight: 1.2
  metric-small:
    fontFamily: IBM Plex Mono
    fontSize: 18px
    fontWeight: 500
    lineHeight: 1.3
  code:
    fontFamily: IBM Plex Mono
    fontSize: 13px
    fontWeight: 400
    lineHeight: 1.5
rounded:
  none: 0px
  xs: 4px
  sm: 6px
  md: 8px
  lg: 12px
spacing:
  "0": 0px
  "1": 4px
  "2": 8px
  "3": 12px
  "4": 16px
  "5": 20px
  "6": 24px
  "8": 32px
  "10": 40px
  "12": 48px
  "16": 64px
components:
  button-primary:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.text-inverse}"
    typography: "{typography.body-strong}"
    rounded: "{rounded.sm}"
    height: 36px
    padding: 12px
  button-primary-hover:
    backgroundColor: "{colors.accent-hover}"
    textColor: "{colors.text-inverse}"
  button-primary-disabled:
    backgroundColor: "{colors.surface-muted}"
    textColor: "{colors.text-secondary}"
  focus-ring:
    backgroundColor: "{colors.focus}"
    size: 2px
  field:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text-primary}"
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    height: 36px
  field-hover:
    backgroundColor: "{colors.surface-hover}"
    textColor: "{colors.text-primary}"
  field-error:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.danger}"
  field-focused:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.focus}"
  panel:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text-primary}"
    rounded: "{rounded.md}"
  app-canvas:
    backgroundColor: "{colors.canvas}"
    textColor: "{colors.text-primary}"
  nav-rail:
    backgroundColor: "{colors.chrome}"
    textColor: "{colors.text-inverse}"
    width: 200px
  nav-rail-collapsed:
    backgroundColor: "{colors.chrome}"
    textColor: "{colors.text-inverse}"
    width: 88px
  inspector:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text-primary}"
    width: 320px
  inspector-minimum:
    width: 280px
  app-shell-compact:
    width: 1100px
  results-minimum:
    width: 600px
  section-gap:
    size: "{spacing.6}"
  floating-surface:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text-primary}"
    rounded: "{rounded.md}"
  divider:
    backgroundColor: "{colors.border}"
    height: 1px
  divider-strong:
    backgroundColor: "{colors.border-strong}"
    height: 1px
  selection:
    backgroundColor: "{colors.accent-soft}"
    textColor: "{colors.accent-hover}"
  interactive-accent:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.text-inverse}"
  caption-muted:
    textColor: "{colors.text-muted}"
    typography: "{typography.caption}"
  status-success:
    backgroundColor: "{colors.success}"
    textColor: "{colors.text-inverse}"
  status-warning:
    backgroundColor: "{colors.warning}"
    textColor: "{colors.text-inverse}"
  chart-dma:
    backgroundColor: "{colors.chart-dma}"
  chart-rvv:
    backgroundColor: "{colors.chart-rvv}"
  chart-npu:
    backgroundColor: "{colors.chart-npu}"
  chart-control:
    backgroundColor: "{colors.chart-control}"
  chart-background:
    backgroundColor: "{colors.chart-background}"
  dark-app-canvas:
    backgroundColor: "{colors.dark-canvas}"
    textColor: "{colors.dark-text-primary}"
  dark-panel:
    backgroundColor: "{colors.dark-surface}"
    textColor: "{colors.dark-text-primary}"
    rounded: "{rounded.md}"
  dark-field:
    backgroundColor: "{colors.dark-surface-muted}"
    textColor: "{colors.dark-text-primary}"
  dark-field-hover:
    backgroundColor: "{colors.dark-surface-hover}"
    textColor: "{colors.dark-text-primary}"
  dark-divider:
    backgroundColor: "{colors.dark-border}"
    height: 1px
  dark-divider-strong:
    backgroundColor: "{colors.dark-border-strong}"
    height: 2px
  dark-interactive-accent:
    backgroundColor: "{colors.dark-accent}"
    textColor: "{colors.chrome}"
  dark-interactive-hover:
    backgroundColor: "{colors.dark-accent-hover}"
    textColor: "{colors.chrome}"
  dark-selection:
    backgroundColor: "{colors.dark-accent-soft}"
    textColor: "{colors.dark-accent-text}"
  dark-caption:
    textColor: "{colors.dark-text-muted}"
    typography: "{typography.caption}"
  dark-secondary-text:
    textColor: "{colors.dark-text-secondary}"
  dark-chart-rvv:
    backgroundColor: "{colors.dark-chart-rvv}"
  dark-chart-npu:
    backgroundColor: "{colors.dark-chart-npu}"
  dark-chart-control:
    backgroundColor: "{colors.dark-chart-control}"
---

## Overview

AxisSim is a serious architecture-exploration instrument. The interface should look like a precision engineering workbench: clear, restrained, data-led and credible. Users explore tradeoffs among RVV processing, NPU throughput, SRAM/memory bandwidth, scheduling, deadlines, and energy. Prioritize a fast and comprehensible experiment-to-insight workflow over decorative dashboard components. Never imply a modeled estimate is measured silicon performance.

Design personality: editorial clarity + laboratory precision. The brand should feel authoritative without feeling institutional or retro. Provide equally complete light and dark workspaces, follow the operating-system preference by default, retain dark graphite application chrome, and use controlled teal interactive accents. Do not produce a neon dashboard, unnecessary gradients, glassmorphism, faux 3D, random glows, or badge-heavy SaaS aesthetics.

Target users: computer architects, silicon/embedded engineers, performance-modeling practitioners, technical reviewers, and students studying architecture tradeoffs.

## Colors

Use `chrome` for navigation chrome, `canvas` for the workspace background, `surface` for focused analytic panels, `surface-muted` for quiet controls, `border` to divide data, and `accent` only for interactive emphasis and selected states. `success`, `warning`, and `danger` always communicate semantic status. Chart categorical colors correspond to stages and are not additional brand accent colors.

Never encode stage identity or performance interpretation with color alone: provide direct labels, units, tooltips, or patterns. Contrast target is WCAG AA for legible UI labels and control text. Use `text-secondary` against white or the main canvas rather than pale low-contrast gray.

## Typography

Use IBM Plex Sans for headings, controls, and narrative explanations, and IBM Plex Mono for precise metrics, tabular data, manifests, trace labels, and code. This supports fast control scanning and directly labeled engineering figures. Avoid faux-bold or additional competing type families.

Numeric values should be tabular and align on decimal points where practical. Always include units next to quantities, never only in a nearby legend. Use sentence-case headings and explicit labels: "Mean latency", "Energy / frame", "DRAM bandwidth". No unexplained uppercase metadata walls.

## Layout

Desktop-first Qt Widgets interface. Proposed dimensions are starting constraints, not fixed window sizes: navigation rail roughly 180–220 px, editable configuration inspector roughly 280–340 px, remaining width for results, charts and technical insight. Minimum comfortable layout width 1100 px. At narrower sizes collapse the nav rail to icons and move configuration into a collapsible inspector; avoid hidden or clipped essential controls. Persist panel sizes where feasible.

Use the spacing tokens above, with 24–32 px separation between major panels and 8–16 px within controls. Give each major work region a complete, clearly contrasted perimeter and use stronger outlines for focused, selected, or interactive regions. Avoid nested outlines that do not communicate hierarchy. Avoid vast blank regions: fill the workspace with information users can inspect, not stretched text boxes. Align labels and unit baselines. Provide clear hierarchy from scenario -> experiment status -> KPIs -> diagnostics -> provenance.

Default overview: (1) one compact command bar with scenario, export, and run action, (2) four inline measurements, (3) detailed frame-zero pipeline timeline, (4) direct-label energy allocation, (5) a narrow evidence-backed bottleneck margin, and (6) compact provenance/model-boundary footer. Advanced configuration is available in Experiment Lab. Do not show an oversized raw manifest as the primary interaction.

## Elevation & Depth

Use flat backgrounds and 1-pixel dividers for primary structure. Add very restrained soft shadow only to true floating UI such as dialogs and context menus. Elevation means something is temporarily above the canvas; it is not an automatic property of every card. Avoid soft blurry cards and gradients.

## Shapes

The workbench uses gently squared corners: 6px controls, 10px panels, 4px small chips. Avoid full pill shapes for every button, giant rounded boxes, or incompatible border radii. Focus and selected states should be clearly visible without drastic movement.

## Components

**App shell.** Single persistent navigation with Overview, Experiment Lab, Compare, Timelines, and Model & Evidence. The currently active view has an unambiguous selected state. Keep the scenario selector and Run action discoverable in each relevant analysis view.

**Scenario editor.** Offer labeled, validated input controls grouped by Compute, Memory, Workload, Scheduling, and Energy. Show units and practical descriptions; keep a Raw Manifest tab for expert edits. Track unsaved and unapplied changes. Running should use the intended configuration, not silently stale results.

**Metric panel.** Label + monospace value + unit + optional baseline-relative delta. Distinguish improvement from regression per metric; for latency/energy lower may be better, for throughput/deadline success higher may be better. Never represent a difference as improved solely because it is positive. Show the result source/run ID on inspection.

**Pipeline timeline.** Where trace data is available, show real starts, ends, concurrency and resource activity; do not infer a Gantt chart from unrelated stage KPIs. Support hover/selection and precise times with units. If unavailable, provide a labeled analytical summary instead.

**Energy breakdown.** Separate total energy from constituent shares, use consistent stage colors and explicit percentages/units. Clearly state if values are per frame or aggregate across a run.

**Comparison.** Side-by-side baseline and candidate, meaningful absolute and relative deltas, constant inputs clearly identified. Parameter sweeps use real emitted CSV/JSON data; Pareto/frontier labels only when algorithmically established. No fabricated chart points.

**Validation & provenance.** Every result view communicates "Modeled estimate", version, input manifest, and run provenance. Where available, link to Ara reference data and explain that validation is bounded to a compute envelope rather than full silicon power/thermal/timing signoff. Keep caveats readable but not visually overwhelming.

**Interaction states.** Define default, hover, pressed, focus, disabled, loading, success, error, and empty states for every interactive component. Focus indicators must be keyboard-visible. Display precise validation errors next to the field, and a useful no-run-yet empty state. Disable Run while its execution cannot be safely repeated and show progress/cancel behavior based on actual CLI capabilities.

**Motion.** State transitions may use subtle opacity or position changes of 160–240ms. No gratuitous chart entrance animation or attention-seeking loading effects. Respect reduced-motion preferences where supported. Data should not re-animate on every small UI update.

## Do's and Don'ts

Do present user intent, clear analysis, reproducibility, and honest assumptions. Do preserve the C++ engine/CLI and treat its JSON/trace outputs as truth. Do provide direct scientific units, readable tables, and error states. Do use one centralized Qt theme/token adapter so style values are not copied across source files. Do visually inspect screens at 1100, 1440, and 1920 px and at a high-DPI scaling setting.

Do not redesign by merely swapping CSS/QSS values or inventing a new arbitrary layout each session. Do not replace actual simulation output with placeholder numbers, fabricate benchmark claims, add unrelated marketing graphics, hide critical settings, or imply experimental improvements are general silicon conclusions. Do not silently migrate from Qt Widgets to QML or to a web frontend; that requires a separate explicit architecture decision.
