# M7.1 — Functional Matrix Command and State Model

**Status:** Implementation of the M7.1 functional milestone. This is an internal AxisSim command model; it does not implement or claim compliance with the RISC-V AME draft or Apple AME.

## Scope

M7.1 executes small, explicit matrix-command sequences over caller-provided tensors. It verifies command legality, row-major slicing, operand and accumulator lifetimes, bank-capacity checks, and signed arithmetic. It adds no timing, throughput, transfer service, memory-system, energy, power, area, host-dispatch, or GUI behavior. M6 GEMM equations and all existing CLI, Python, and Qt interfaces remain unchanged.

## Work and input contract

The command model accepts positive dimensions `(M,N,K)` and positive tile extents `(Tm,Tn,Tk)`. A and B are exact-size row-major contiguous signed 16-bit tensors of `M*K` and `K*N` elements; C is an exact-size row-major signed 32-bit tensor of `M*N` elements. The matrix operation is `C[i,j] = sum_k(A[i,k]*B[k,j])`, initialized by the explicit accumulator clear. Products are exact signed-i16 products represented in signed i32, and each accumulation wraps modulo `2^32`. No rounding is performed.

Construction rejects zero dimensions or tile extents with `std::invalid_argument`, unrepresentable element counts with `std::overflow_error`, and spans whose lengths do not exactly match the tensor shapes with `std::length_error`. These are rejected before any command state exists.

## Command record and canonical fields

Each fixed-field `MatrixCommand` contains a sequence ID, opcode, output tile indices, K panel index, and valid M/N/K extents. The one-slot model has no user-selected slot field. Unused fields must be zero and otherwise produce `unused_field_nonzero`.

| Opcode | Required extents/identity | Unused fields that must be zero |
|---|---|---|
| `clear_accumulator` | Output tile, `valid_m`, `valid_n` | `k_panel_index`, `valid_k` |
| `load_a` | Output tile, K panel, `valid_m`, `valid_k` | `valid_n` |
| `load_b` | Output tile, K panel, `valid_n`, `valid_k` | `valid_m` |
| `matrix_mac_accumulate` | Output tile, K panel, all three extents | None |
| `store_accumulator_tile` | Output tile, `valid_m`, `valid_n` | `k_panel_index`, `valid_k` |

Supplied extents never override computed extents. The model checks them against the clipped tile shape derived from the workload and mapping.

The operand slots are checked against the derived tile shape, not against the caller’s command fields. Therefore an incorrect caller-supplied used extent is reported as `invalid_extent`; `shape_mismatch` is reserved for an operand slot whose retained identity or shape conflicts with the independently derived current tile/panel.

## Traversal and row-major offsets

Output tiles advance in M-major then N order. Each tile follows `ClearAccumulator`, then for each K panel in ascending order `LoadA`, `LoadB`, `MatrixMacAccumulate`, and finally `StoreAccumulatorTile`. A and B slots are released after the matching MAC. The accumulator is retained through all K panels and released after its output tile is stored. Each accepted command consumes exactly the next sequence ID, starting at zero.

For indices `(im,in,ik)`:

```text
m0 = im * Tm                 n0 = in * Tn                 k0 = ik * Tk
vm = min(Tm, M - m0)         vn = min(Tn, N - n0)         vk = min(Tk, K - k0)
A[(m0+r)*K + (k0+k)]
B[(k0+k)*N + (n0+c)]
C[(m0+r)*N + (n0+c)]
```

All dimensions, products, offsets, spans, and capacity-byte calculations are checked before state/output mutation. The 5×7×3 fixture with 4×4×2 tiles has final tile/panel extents `(vm,vn,vk)=(1,3,1)`. Its A, B, and C row-major starting element indices are respectively `4*3+2=14`, `2*7+4=18`, and `4*7+4=32`. Tests check the final output values against an independent scalar oracle and hand-derived final-row values.

## Capacity and state

The context declares independent capacities for one A tile, one B tile, and one accumulator tile. Each command checks the live tile’s required bytes before retaining it: signed-i16 A/B elements use two bytes each and signed-i32 accumulator elements use four. These are abstract state-capacity limits only; they are not SRAM, area, or energy estimates. Exact-fit capacities pass; one-byte-short capacities fail with `capacity_exceeded`.

## Failure atomicity and error contract

A rejected command leaves A and B slot contents/identities, accumulator values/state/panel, C output, and the next expected sequence ID unchanged. Invalid commands do not consume IDs, so a corrected command can be retried at the same ID. Validation completes before mutation. Allocating operations prepare replacement buffers before commit; store checks every destination before its first output write. Sequence increment overflow is rejected before mutation.

Stable errors are: `sequence_mismatch`, `unexpected_opcode`, `unused_field_nonzero`, `wrong_tile`, `wrong_k_panel`, `slot_busy`, `missing_operand`, `shape_mismatch`, `invalid_extent`, `source_bounds`, `accumulator_not_complete`, `duplicate_store`, `capacity_exceeded`, and `arithmetic_overflow`. Error selection is deterministic: sequence, opcode, canonical unused fields, derived context/extent bounds, tile, K panel, state/lifetimes, used extents, source bounds, capacities, and sequence increment overflow. One explicit state exception is a repeated store to the immediately completed tile: after validating its tile coordinates are in-range, return `duplicate_store` before the generic next-tile mismatch.

Tests exercise rejected clear/load/MAC/store commands, ensure the sequence is unchanged, retry with the same ID, and verify the final tensor so retained slots and accumulator values are checked behaviorally. A rejected store leaves sentinel output unchanged. The model does not roll back previously accepted commands if a later command is rejected; atomicity applies to the rejected command itself.

## Verification

`tests/matrix_architecture_tests.cpp` uses a separate scalar GEMM oracle and test-owned command generator. It covers 1×1 and rectangular GEMMs, partial M/N/K tiles, multiple K panels, modulo overflow, traversal order/IDs, all opcode unused-field rules, wrong tile/panel identities, slot lifetimes, capacity boundaries, malformed sequences, invalid contexts, and rejected-command atomicity. In particular, the 5×7×3 workload with 4×4×2 tiles verifies the final `(1,3,1)` clipped extents and row-major addresses.

The CTest name is `matrix_architecture_cpp_contract`. Ubuntu full CTest runs it with the full suite; Windows and macOS portability jobs include it in their existing C++ contract filters. Passing these tests establishes functional and deterministic software behavior on tested builds, not physical matrix-engine correctness or performance.

### Local verification record

Baseline: accepted `main` commit `2b6429fc6bcc0d0d9013d89888548c5a8b7d05dd`. Local build: AppleClang 21.0.0, Release, Qt 6 GUI enabled. The focused `matrix_architecture_cpp_contract` passed. The complete local CTest suite passed **76/76**, including GUI checks, `legacy_study_baselines_reproduce` (M0 baseline fixture), and `m5_replay_verifier_contract`. A direct AddressSanitizer/UndefinedBehaviorSanitizer build of the new C++ source and test also passed.

The M5 verifier contract test validates verifier behavior; it does not replay the complete accepted M5 archive. That archive was unavailable in the local environment, so no full 555-artifact M5 replay is claimed. Configured Linux, Windows, and macOS CI results are recorded in the milestone acceptance report after branch CI completes.

## Explicit limitations

No AME-specific instruction encoding/state, standard compliance, measured hardware behavior, physical memory transfer, concurrency, compute/transfer overlap, timing, throughput, power, energy, area, reliability, or physical bank architecture is modeled. M7.2 and all later milestones remain separately gated.
