# M7.3 Research Closure

**Disposition:** M7.3b accepted as **DEFER**. No Ara timing protocol or implementation is authorized.

**Accepted AxisSim baseline:** `main@7cff8b91a82af79218e4ce1027a39578c5b4d45d`

**Pinned Ara revision:** `34bd3bc152421b4601a7bf3d6e8e91ffb545c99e`

## Decisions

- **M7.3a Gate 0 is REJECTED as specified.** Its frozen endpoint requires the final MFPU result grant, all required completion evidence, and `ara_resp_valid` on the same edge. The pinned sequencer does not emit `ara_resp_valid` for vector-destination `vwmacc.vx`; replacing it with a different signal would change the rejected contract.
- **M7.3b is DEFER.** Other observable Ara intervals exist, but neither currently supports an action-changing project decision worth the environment and implementation cost.
- Do not treat the rejection or deferment as RTL measurement, validation, or approval of an alternative protocol. No instrumentation, timing simulation, or 16-case study was run.

## What the two alternative intervals mean

| Candidate | Boundary | Meaning and limits |
|---|---|---|
| **A. MFPU-local window** | Start at the first target EW32 multiplier input handshake (`valid_i && ready_o`); end at the last attributable result grant across the participating lanes (`mfpu_result_req_o && mfpu_result_gnt_i`). Count rising edges as `end_edge - start_edge` using `[start_edge,end_edge)`. | Measures input acceptance through MFPU result commitment to the vector register file. It includes pipeline fill/drain and any result-queue or grant stalls. The multiplier interface itself carries no instruction ID, so attribution would need an explicitly validated monitor-side tag or strict single-target isolation. It is not a pure arithmetic-capacity interval. |
| **B. Per-ID backend-completion window** | Use the same start; end on the per-ID `pe_vinsn_running_o[id]` transition from active to inactive after PE completion responses have propagated through the sequencers. | Measures a later backend-completion boundary across the participating processing elements. It includes registered completion propagation after MFPU commit. It is distinct from A and is not an architectural CPU-retirement timestamp. |

For VWMACC, the dispatcher sets `use_vd=1` and selects signed widening `VMACC`. The sequencer asserts `ara_resp_valid_o` for memory acknowledgments or scalar-result cases, not this vector-destination operation. The MFPU emits per-ID completion when its commit count reaches zero; lane and top-level running-state updates add registered completion propagation. Pinned-source excerpts are preserved on the recovery branch at `pinned-source-context/ara/hardware/src/ara_dispatcher.sv:1920-1928,2098-2105`, `pinned-source-context/ara/hardware/src/ara_sequencer.sv:86-101,367-369,509-514,554-571`, `pinned-source-context/ara/hardware/src/lane/vmfpu.sv:438-457,2190-2229`, and `pinned-source-context/ara/hardware/src/lane/lane_sequencer.sv:273-276,955-976`.

Neither candidate should be compared for exact equality with the original M6.3 cycle equation. A could separately expose accepted input-beat spacing and result-commit delay, but its aggregate window includes both. B includes additional lifecycle delay. They answer different questions.

## Why Ara work is deferred

M6.3 estimates an analytical RVV compute envelope: per fixed-VL chunk, `K × max(ceil(VL / (lanes × P)), R)`, plus any explicitly modeled setup. In the frozen hypothesis, `P=2`, `R=1`, and setup is zero. `P=2` is a structural hypothesis based on the EW32 datapath; `R=1` is an analytical recurrence assumption. Neither has been established as sustained Ara performance by this evidence.

The model does not simulate Ara instruction dispatch, dependency issue, multiplier pipeline timing, result-queue backpressure, VRF commitment, or whole-instruction completion. Its equations and hand-derived tests verify analytical accounting; they do not validate physical or implementation-specific timing. M7.1's functional matrix command/state model and M7.2a's synthetic accounting remain separately accepted. M7.2b remains deferred; its 32 synthetic requests remain unexecuted.

An Ara experiment would become useful if the project first names a decision that depends on an Ara-specific RVV profile—for example, deciding whether to add a separately versioned Ara timing profile rather than retaining the current abstract estimate. No such decision is currently approved. The recorded environment is also blocked: approximately 13 GiB free against the documented 20 GiB minimum, missing pinned Verilator, built RISC-V LLVM, and Bender, and no provisioned Linux host or usable Docker daemon. That setup burden is not justified without the named decision.

## Conditions for reopening

Reconsider Ara timing only after all of the following:

1. The owner identifies the specific modeling or architecture decision the measurement can change and whether the goal is Ara-specific characterization or a broader claim.
2. A new protocol keeps local MFPU service and per-ID backend completion as separate observables, specifies event attribution and edge sampling, and does not assume either interval equals the M6.3 equation.
3. The pinned Linux/toolchain environment is provisioned and its identities are recorded.
4. Synthetic event fixtures and independent expected observations are specified, and a fresh EGM design review resolves implementation-boundary, attribution, and measurement claims.
5. A separate implementation authorization is given.

Until then, keep the M6.3 assumptions analytical and unvalidated against this Ara revision. Do not tune `P` or `R` from a future single measurement without a separately justified model change.

## Preserved evidence and integrity

The rejected evidence is preserved on `research/m7-3a-gate0-recovery@1aaa215848d42c23f2655eea5a0434b6cb237fb8`, parented by the accepted baseline above. Under `docs/architecture/m7_3a-gate0-recovery/`, the recovery branch contains the reconstructed proposal, fresh EGM reports, pinned-source excerpts, manifests, environment record, and rejection decision. The original standalone proposal was unavailable; the recovered transcript is not proof of the original file's hash.

Key identities:

- Reapproval manifest SHA-256: `576895d49358424c443f141c77054583c7f459cf04a77c9cb4aaffd44eb55e86`
- Rejection record SHA-256: `2c8ed3a4bd1223a1f3dd1795ee453083f0e218ca3c8805ddd9f3b973d270f473`
- Reviewed rejected-draft SHA-256: `5643447a7b0f59865b4145e9d0dffb1b3e1415c1ea142cbe75570631bf6151c0`
- Transcript-recovered proposal-body SHA-256: `b039a9f9c10589816c96f6953297714e414f9d70a99914fbbb8f8e4d4fb587c5`; the original standalone file hash is unknown.
- Private recovery Git bundle SHA-256: `60ac44f56b1611c550e2645c0a86c181d324d6cafda3f579acd865f606025b21`

Both recovered evidence manifests and the Git bundle verified during this closure. The private owner-controlled archive reports uploaded with no sync pause or unresolved conflict; independent restore from another device has not been performed. The private archive is supplementary evidence, not a dependency for building or using AxisSim.

The historical EGM Stage 9 review found the frozen endpoint Blocker and unresolved implementation-readiness Majors. This closure only records the already accepted rejection and defer decision; it does not claim fresh EGM convergence or approval of a new protocol.
