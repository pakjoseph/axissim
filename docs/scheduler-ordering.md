# Scheduler ordering and compatibility

AxisSim has two independent scheduler settings:

- `scheduler_policy` selects the resource arbitration rule: `fixed_priority`, `fcfs`, or `edf`.
- `scheduler_ordering_policy` selects how otherwise equivalent events are ordered: `legacy_implicit` or `deterministic_v1`.

The second setting does not replace the first. Existing configuration files that omit `scheduler_ordering_policy` resolve to `legacy_implicit`; their files, CLI output, and schema 1.2 JSON remain unchanged. The historical M0 and accepted M5 artifacts remain preserved as references. The legacy policy retains its prior ordering for valid inputs and therefore only promises the recorded ordering in the original reference environment. It is not a cross-toolchain scheduling contract.

New cross-platform work should opt in explicitly:

```sh
./build/axissim run configs/workloads/mnist12_profiled.conf \
  --set scheduler_ordering_policy=deterministic_v1 \
  --json experiments/mnist12_deterministic.json \
  --trace experiments/mnist12_deterministic.csv
```

Deterministic runs print `scheduler ordering: deterministic_v1`, emit schema 1.3 JSON with the same policy field, and include the setting in the candidate evaluator's effective configuration. Unsupported values are rejected. Candidate configuration identities already cover the full override object, so the selected ordering policy changes that identity.

## `deterministic_v1` ordering contract

All sequence counters are zero-based, monotonic, local to one scheduler invocation, and checked before increment overflow. Event creation and enqueue sites iterate stable vectors or numeric frame/control indices; the scheduler does not derive order from pointer values, unordered-container iteration, or heap stability.

1. **Future/ready queue:** earliest `ready_us` first, then ascending enqueue sequence.
2. **Completion queue:** earliest `end_us` first, then ascending dispatch sequence. Dispatch sequence represents the order tasks were submitted to their resources.
3. **Resource arbitration:** preserve the selected `scheduler_policy` criteria first. `fixed_priority` keeps lower numeric task priority first; `edf` keeps earlier absolute deadline first; `fcfs` keeps earlier ready time first. The existing ready-time criterion remains next where applicable. Remaining ties resolve by ascending enqueue sequence.
4. **Timestamp boundary:** at a scheduler timestamp, already-enqueued due tasks are moved to resource queues in future-queue order; all due completions are then processed in completion-queue order; ready resource queues are dispatched in the existing DMA, RVV, NPU resource-index order. Tasks released by those completions receive new enqueue sequence IDs and are considered on the next scheduler iteration, including another iteration at the same modeled timestamp.
5. **Dependency release:** completions at equal timestamps use completion sequence order. A completed profile operation visits its successors in the stable profile operator order. An operation with several dependencies enters the future queue only after its final predecessor completes.

The comparator uses a total timestamp ordering for malformed NaN keys (NaNs sort after numeric times and form one timestamp-equivalence class), so the comparison remains strict-weak-ordering-safe. Valid workloads should produce finite timestamps. Equal finite timestamps use exact binary64 equality; no tolerance is applied to scheduler event times.

## Model and evidence versioning

`legacy_implicit` and `deterministic_v1` are different scheduler semantics. Their latency and trace differences are not automatically performance improvements or regressions. The legacy M0 fixture remains immutable and is compared byte-for-byte on the recorded macOS arm64 reference platform. On other platforms, the legacy path is tested for same-platform repeatability without pretending its output equals the macOS snapshot. `deterministic_v1` has its own portable result and 720-task trace fixture at `tests/reference/deterministic_scheduler_v1.json`, exercised on Linux, Windows, and macOS.

The deterministic MNIST reference has a different frame-latency assignment from the legacy macOS M0 record. Its measured model output is retained in a separate fixture and is not written back to `tests/reference/legacy_study_baselines.json` or the accepted M5 study artifacts.

Replaying M5 with a changed executable necessarily creates a new `simulator_build_identity` and new candidate identifiers, so its raw canonical artifact comparison is expected to differ. A replay was run for this policy change: all 168 candidate result records and all 88 retained task traces were identical after removing only the changed executable/candidate identity fields. The accepted M5 artifact bundle itself was left untouched; this compatibility comparison does not replace it with a new M5 snapshot.
