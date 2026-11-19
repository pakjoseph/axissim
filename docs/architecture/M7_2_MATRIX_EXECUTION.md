# M7.2a Matrix Execution Accounting

M7.2a is a synthetic accounting and verification layer. It adds no production command, study runner, or architecture recommendation. Capacity and issue-cycle values used in tests are mathematical fixtures, not hardware characterization.

## Ownership

The calculator is implemented in `src/matrix_execution.cpp` and declared in `src/matrix_execution.hpp`.

- M6 remains authoritative for the workload contract, RVV estimate, tiled-matrix compute cycles, aggregate working-set fit, logical traffic, and optional serialized-service equation. M7.2a calls `estimate_rvv_row_gemm`, `estimate_tiled_matrix_gemm`, and, when explicitly requested, `estimate_serialized_backing_store`.
- M7.1 remains authoritative for matrix opcode names, command lifecycle, and one-slot A/B/accumulator state semantics. M7.2a counts that schedule; it does not execute tensor commands or assign per-command timestamps.
- M7.2a owns explicit input validation, per-bank capacity checks, closed-form command counts, issue-cycle bookkeeping, and result classification.

No M6 mapping or serialized-service equation is copied or changed.

## Explicit input contract

`MatrixExecutionRequest` cannot be default constructed. It requires the workload, reference clock, every RVV and matrix mapping value, three bank capacities, five opcode issue costs, and an explicit optional serialized setting. The mapping, capacity, cost, and serialized input records also cannot be default constructed. This prevents a missing field from silently taking an implicit zero.

The clock must equal `kGemmV1ClockHz` (1 GHz). It is explicit in each request but is not configurable in M7.2a. Both mappings require recurrence one and zero setup costs. RVV and matrix mappings must declare equal nominal MAC slots per cycle. The three bank capacities must sum exactly to `matrix_mapping.local_capacity_bytes`; zero total capacity is invalid. An optional serialized record must have positive bandwidth and explicitly includes batch setup cycles, including when that value is zero.

The one-slot matrix state requires:

- A bytes = `min(M,tile_m) × min(K,tile_k) × 2`
- B bytes = `min(K,tile_k) × min(N,tile_n) × 2`
- Accumulator bytes = `min(M,tile_m) × min(N,tile_n) × 4`

All products and sums use checked unsigned arithmetic. M6 aggregate fit and each M7 bank fit are both required for a complete matrix result.

This footprint is limited to the live A tile, B tile, and accumulator tile. It
does not include scratch space, cache storage, extra accumulators, double
buffers, hidden registers, or reuse retained across output tiles. The byte
values are synthetic capacity inputs for accounting checks, not descriptions
of physical SRAM banks or total silicon storage.

## Single-work-unit scope and alternatives

Each request represents exactly one shape-only GEMM work unit. The returned
`combined_matrix_command_compute_cycles` is the aggregate analytical command
issue plus M6 compute interval for that work unit. It does not contain per-tile
or per-command timestamps. A future batch runner would have to define release,
queueing, and work-unit aggregation separately; none is implied here.

The calculator intentionally uses the following alternatives:

- It calls the existing M6 mapping helpers instead of copying their arithmetic,
  tail, or traffic equations.
- It derives aggregate opcode counts from the accepted M7.1 lifecycle rather
  than running tensor-backed commands and assigning invented timestamps.
- It exposes a direct C++ API for synthetic verification rather than adding a
  production CLI, Python runner, serialized schema, or GUI flow without an
  identified consumer.

These choices keep the change independently verifiable while limiting it to
accounting behavior that can be checked against hand-derived fixtures.

## Command and cycle accounting

Let `O = tiles_m × tiles_n` and `Q = tiles_k`, taken from the M6 matrix estimate. The deterministic M7.1 command schedule has:

- Clear accumulator: `O`
- Load A: `O × Q`
- Load B: `O × Q`
- Accumulate: `O × Q`
- Store accumulator tile: `O`

Each explicitly supplied issue cost is integer cycles per accepted semantic command. Issue cycles are multiplied by the corresponding command count and summed with checked arithmetic. The matrix command/compute interval is M6 base matrix compute cycles plus issue cycles. It is one whole-workload analytical estimate, not a per-tile timeline, measured latency, or contention occupancy.

If serialized service is requested, the M6 helper is called for both arms with identical bandwidth and batch-setup inputs. The matrix engine field is named `serialized_engine_service_cycles` because it includes M6 base compute plus the M7.2a issue total. Serialized service is an optional secondary calculation and is never added again to the command/compute interval. Nonzero matrix issue costs are asymmetric with the current RVV model, which does not price RVV instruction issue; therefore serialized totals cannot rank the two arms.

## Independent controls

The focused `matrix_execution_cpp_contract` tests hand-derived controls:

1. 4×4×4, 4×4×4 tile, P4, 1 GHz, 128 B aggregate capacity split A32/B32/Acc64: M6 matrix compute 16 cycles, logical A/B/C 32/32/64 B, five commands, and zero-cost M7.2a interval 16 cycles.
2. Same total capacity split A16/B48/Acc64: M6 aggregate fit passes, the A bank fails, and no M7.2a interval is returned.
3. One issue cycle per each command type: five itemized additional cycles and a 21-cycle matrix command/compute interval.

The paired RVV estimate for the square fixture is 16 cycles but carries A/B/C traffic of 32/128/64 B (224 B total) under M6's per-useful-MAC B-read assumption. The matrix mapping carries 32/32/64 B (128 B total). Tests preserve this M6 traffic difference and verify serialized outputs of RVV 3/16/1/20 and matrix 1/21/1/23 cycles at 64,000,000,000 B/s, zero setup, and one-cycle issue costs. These are equation checks only.

The 5×7×3, 2×4×2, P4 fixture verifies M6 active-output tail accounting: matrix base 30 cycles, traffic 60/126/140 B, state 8/16/32 B, and counts 6/12/12/12/6. The paired RVV mapping yields 30 cycles and traffic 30/210/140 B. An independently generated canonical trace checks stable M-major, N-major, ascending-K ordering, matches the closed-form counts, and is accepted by the M7.1 state machine; it assigns no timing to those commands.

## Result and error behavior

- `complete`: includes both M6 estimates, bank-fit details, command counts, issue cycles, combined matrix command/compute cycles, and both serialized records only if requested.
- `aggregate_capacity_exceeded`: includes the RVV estimate and bank diagnostics; no matrix base estimate, command counts, issue cycles, combined interval, or serialized records.
- `bank_capacity_exceeded`: retains the M6 matrix estimate and diagnostic command counts but has no issue cycles, combined interval, or serialized records.
- Invalid inputs throw `std::invalid_argument`; checked arithmetic overflow throws `std::overflow_error`. Invalid requests do not return partial results.

No result encodes a winner or recommendation. This code does not model physical SRAM or DRAM, transfer latency, power, energy, area, AME instructions, CPU coordination, queues, or overlap. M7.2b comparative screening remains blocked until the owner supplies a named architecture decision and intended capacity/command assumptions with traceable provenance or an explicitly bounded hypothetical range, followed by separate approval.

## Validation

Configure and build with the project's standard CMake commands. Run the focused contract with:

```sh
ctest --test-dir build --output-on-failure -R 'matrix_(architecture|execution)_cpp_contract'
```

The full CTest suite is required before integration. Windows and macOS run both targeted matrix contracts in the existing C++ portability jobs; Ubuntu runs full CTest. Python jobs remain unchanged and do not independently validate the M7.2a C++ library.

## Implementation checkpoint and rollback

M7.2a is one bounded implementation phase. Its checkpoint is the complete
allowlisted C++ calculator, focused hand-derived tests, CMake/CI registration,
and this scope/limitations document on a dedicated unmerged branch. Acceptance
requires the focused tests, full available CTest (including GUI regression),
the existing compatibility checks, the configured cross-platform CI matrix,
and a clean independent implementation review. If a check fails or any change
requires modifying M6 equations/contracts, M7.1 command transitions, accepted
artifacts, public CLI/Python/Qt behavior, or files outside the allowlist, stop
and revert only the M7.2a branch changes. M7.2b remains separately gated even
when M7.2a passes.
