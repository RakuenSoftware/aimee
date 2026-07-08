# Agentic supervised SWE-bench (#987) — live-wiring status

Status of wiring the 7 live `NotImplementedError` stubs in the agentic supervised SWE-bench
harness. Honest split of **implemented / locally-verified / validation-pending**. Written
2026-07-08 (autonomous session, JBailes). The proposal is
`docs/proposals/pending/agentic-supervised-swebench.md`.

## Summary

The pure S0–S6 cores were already merged + unit-tested. This work wired every live stub onto the
**`aimee delegate` transport + job-id attribution** model (the deployment-verified correction from
`swebench_live_attribution.py`, not the theoretical `/v1/runs` EMPTY-polarity model). All
reproducibility-critical logic is pure and unit-tested; the live glue is thin, composes the tested
cores, and takes injected seams so it is exercised in CI with a fake fleet.

**Bench tests: 593 pass** (`python3 -m unittest discover -s benchmarks/tests`), +~35 new. The S0–S6
runbook fast-check (161 tests) is green.

## What was wired (previously `NotImplementedError`)

| stub | module | status |
|---|---|---|
| `run_agentic_loop` | `swebench_agentic_harness.py` (S1) | wired: provision → dispatch `code --tools --worktree` → extract patch (workspace diff, else returned diff) |
| `run_arm_a` | `swebench_arm_runner.py` (S2) | wired: composes the loop + job-id token headline; two wall-clocks |
| `run_arm_c_supervised` | `swebench_supervision.py` (S3) | wired: Option A — N concurrent workers + tools-OFF supervisor + deterministic best-of-N + gated escalation |
| `run_suite` | `swebench_suite.py` (S5) | wired: run-plan → arms → official grade (lease + retry) → K-aggregate → report; injected seams |
| `run_live_matrix` | `swebench_live_attribution.py` (S0-live) | wired: dispatch primary+worker probe → verify L1–L4 over the real schema |
| `_invoke_v1_runs`, `_invoke_agent_shell` | `swebench_transport_verify.py` (S0) | intentionally left raising, message updated to redirect to the sanctioned delegate transport (the `/v1/runs` EMPTY-polarity model is superseded) |

New module: `swebench_live_transport.py` — the single live transport (argv build with the
tools⇒worktree invariant, dispatch/poll with an injectable runner, job-id token split, supervisor
tool-row assertion). Fully unit-tested.

## Verified from this box

- **Fleet transport works** via the .254 gateway: dispatch → `job_id` → poll → terminal `result`
  (real `GLM-5.2` jobs 25/26). `aimee insights` confirms the token ledger is real and reachable in
  aggregate (my glm-5.2 calls appear).
- **All pure logic + the dispatch/poll loop** are unit-tested with a fake fleet (593 green).
- **Multi-provider consultation** (`aimee delegate aggregate`) engages all 6 providers
  (mimo-v2.5-pro, GLM-5.2, gpu-mid, MiniMax-M3, mistral, codex; 0 failed).

## Validation-pending (needs hardware not reachable from this box) — **hypothesis, unverified**

1. **End-to-end graded run + claim gate (acceptance #5/#6).** Requires the official SWE-bench
   grader on **CT 101** (real docker + py3.11). Verified 2026-07-08 that the `.253` host docker is
   the **LXC2Docker shim** (mangles images) with py3.13 and ~14G root — unsuitable; confirms the
   documented CT-101-only rule. Grading was **not executed**; `official_grade_fn` parses the report
   `resolved_ids` but that parse is validated against the documented schema, not a live report.
2. **Real-ledger token attribution.** The ledger lives on the fleet host
   (`/mnt/media/.plugins/aimee-server/server/home/aimee.db` on .254); this box reaches the fleet via
   the gateway but has no SSH/DB access. The job-id split logic is unit-tested; the numbers against
   the real ledger are pending.
3. **Workspace co-location.** Arm-A/C workers must edit a repo checked out at `base_commit`
   co-located with where the delegate executes (server-side on the fleet). The provisioning +
   extraction functions are tested; the co-located server-side execution is the operator step.
4. **The claim gate is human-gated by design** (S6 criterion 6: an independent reviewer who did not
   author the harness) — it can never be auto-satisfied autonomously.

## Why the proposal is NOT moved to `done`

Acceptance criteria #5 and #6 are deployment-tier: the official graded suite on CT 101 across
Benchmark 1 (K=10) **and** Benchmark 2, plus an **independent** claim-gate reviewer. Those require
the co-located fleet + CT-101 real docker + a human reviewer and cannot be completed from this
client box. The code is wired and CI-green; the graded run + gate remain the operator's step.
