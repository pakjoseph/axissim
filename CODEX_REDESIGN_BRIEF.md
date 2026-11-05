# Codex task: AxisSim Precision Lab redesign

Please redesign the existing AxisSim Qt desktop interface as a coherent, premium *architecture workbench* while preserving every functioning simulation capability and the C++ engine.

Start by reading `README.md`, `DESIGN.md`, `AGENTS.md`, relevant Qt sources, and CLI JSON/trace interfaces. If the `redesign-existing-projects` skill is installed, use it as an audit checklist; `DESIGN.md` wins for exact design values and product-specific priorities. Review the three existing UI screenshots shared in our conversation if accessible; otherwise inspect current GUI screens yourself.

**Do not begin by creating multiple aesthetic variants.** First audit the implemented views and states. Produce a concise matrix: existing feature, existing screen/control, visual/UX problem, proposed fix, affected source files, regression risk. Preserve features, not just the most visible baseline page.

**Direction:** Precision Lab: graphite navigation, complete light/dark/system appearance modes, neutral high-information panels, restrained teal interactions, Times New Roman editorial UI typography with IBM Plex Mono for technical data, clearly contrasted full-perimeter outlines, one consistent spacing scale, no fake glassmorphism/neon, and no visualizations with fictional data.

**Target information architecture:** Overview (high-signal results), Experiment Lab (guided and raw manifest input), Compare (baseline versus candidate), Timelines/Sweeps (actual trace and CSV data), Model & Evidence (Ara provenance, estimates and limitations). Implement this incrementally and only split navigation into views that the actual app can support.

**First implementation milestone:** Redesign the core Overview screen, app shell, scenario selection/Run control, KPI components, and configuration inspector; reuse current CLI JSON results. Create centralized, reusable UI components/theme. Keep actual model fields and units. Add meaningful idle, loading, error, invalid-configuration and success states.

**Second milestone:** Improve trace timeline and energy visualization with consistent stage-color mapping, readable scale and labels, and direct interaction; use only emitted data. Add controlled A/B comparisons or reveal existing comparison workflows. Make sure all percentages have clear denominators and every traffic/energy number clearly says per frame or per run.

**Third milestone:** Polish small-screen behavior, keyboard and focus interactions, accessible contrast, data-export affordances and persistence. Add visual regression screenshots/checks if feasible in the chosen Qt toolchain.

**Verification:** Build with CMake/Qt, run CTest and the documented GUI smoke test when available. Inspect screenshots at 1100, 1440, and 1920 px; try high-DPI scaling. Do not claim that a test or screenshot was performed if the environment cannot perform it. Report the implemented files, screenshots, verification results, and remaining limitations.

**Science/credibility constraint:** Every displayed numeric metric must come from the simulator's configured scenario, not from the design mockup. Keep “Modeled estimate” prominent and avoid measured-silicon, timing-closure, yield, or reliability-signoff implications. Do not change simulation algorithms to make the new UI attractive. Do not switch to React/QML or substantially alter the backend without explicit approval.
