# AxisSim agent instructions

## Product and architecture
AxisSim is a deterministic C++ architecture simulator for a hypothetical Physical AI SoC, with a Qt 6 Widgets desktop explorer when enabled. The C++ engine/CLI is the source of truth. Preserve manifest semantics, run/trace/JSON behavior, reproducibility and tests. Do not substitute mock calculations or fabricated benchmarks.

## Design governance
Before ANY UI, chart, component, styling, or UX work, read the repository-root `DESIGN.md` in full. Its token values and component rules override external design-skill aesthetic defaults. Keep typography, colors, spacing, radii, layout, motion and semantic states consistent. If a requested UI change requires a new token, amend `DESIGN.md` intentionally rather than introducing a one-off literal. Do not redesign the brand or invent a theme from scratch during each session.

The third-party `redesign-existing-projects` and `landing-page-design` skills are **audit/quality heuristics**, not authorities for AxisSim's colors, fonts, layout or feature priorities. The product is an engineering workbench, not a marketing landing page.

## Native Qt implementation
Inspect existing frontend code and current Qt/C++ build structure before choosing a styling strategy. Create a centralized theme/token adapter (QPalette, named component styles/QStyle or focused QSS as appropriate) instead of scattered `setStyleSheet` literals. Use real layout managers and size policies, not arbitrary fixed pixel placements. For charts, align meaningful categories/axes/units and use the model's actual outputs. Consider QSS limitations and keep it scoped; avoid a broad framework or frontend rewrite unless separately approved.

## Workflow for every UI task
1. Inspect the relevant source and current screenshots; summarize UX/visual problems in concrete terms.
2. State what screen/component will change and which `DESIGN.md` rules govern it.
3. Make a small, reviewable implementation change. Preserve existing workflows.
4. Build and run relevant CMake/CTest checks. For GUI work, run the documented Qt smoke test if the environment supports it.
5. Capture and inspect the real interface at desktop and narrower sizes, in idle/loading/success/error states. Report untested environments instead of asserting they passed.
6. Recheck modeled-vs-measured copy, numeric units, accessibility, run provenance, and UI state consistency.

## Existing documented commands
```sh
cmake -S . -B build -DAXISSIM_BUILD_GUI=ON
cmake --build build
ctest --test-dir build --output-on-failure
# macOS example, when the Qt GUI target is available:
QT_QPA_PLATFORM=offscreen ./build/AxisSim.app/Contents/MacOS/AxisSim --smoke-test-run
```

Avoid blindly overwriting existing project instructions. Merge this guidance with any existing `AGENTS.md` content and preserve repository-specific constraints.
