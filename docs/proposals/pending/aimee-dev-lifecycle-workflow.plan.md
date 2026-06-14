# Implementation plan: aimee workflow engine

Plan for [`aimee-dev-lifecycle-workflow.md`](aimee-dev-lifecycle-workflow.md)
(roundtable-converged, user-approved). Delivered as **seven sequenced slices**, each
its own PR → roundtable → testing, smallest-blast-radius first. The pure
definition/validator lands first (no delegates, no DB); the live roundtable gate
(W5) is **hard-gated on §0** (`roundtable-panel-composition`) and until that ships is
built and tested against a mock panel only.

## Dependency posture (§0)
The *engine* (definitions, state, validator, non-gate blocks, human gates, autonomy
driver) can be fully built and unit/fixture-tested **without** the panel dependency,
because `gate.roundtable` is exercised against a deterministic **mock panel** in
tests. Only the **live** `gate.roundtable` wiring (W5's "connect to the real
ensemble") requires `roundtable-panel-composition`. So W1–W4, W6, W7 are unblocked
today; W5's live wiring is the one piece that waits on §0 (its mock-tested logic does
not).

## Slice W1 — Definition + validator + **frozen interfaces** (pure; no engine, no DB)
The contract everything else builds on — and the slice that **freezes the seams** so
W2–W6 plug in implementations without re-amending W1. No delegates, no DB, no git —
fully unit-testable.
- The YAML schema for a workflow: block instances (`id`, `block`, `params`, typed
  `in:` handle bindings, `next`/`on_pass`/`on_fail` control edges), the **closed
  artifact type-system** (`proposal|plan|branch|frozen_diff|pr|verdict|approval`).
- **Frozen block catalog (full vocabulary, not just W1's blocks).** The catalog
  registers **every** block type the engine will ever run — `author.proposal`,
  `author.plan`, `implement`, `freeze`, `pr.open`, `merge`, `gate.human` (→`approval`),
  `gate.roundtable` (→`verdict`) — each with its declared typed I/O, in W1. Later
  slices supply *executors*, never new catalog/type entries. This makes `build.yaml`
  (and any future-complete workflow) validate against a **frozen type vocabulary** in
  W1, so the W3/W5 roundtables can't force a W1 amendment PR.
- **Frozen execution seam — `src/workflow/wf_iface.h` (deliberately NARROW).** Freeze
  *only what the engine touches*, so later slices evolve their internals without
  breaking W1's contract test (avoiding "coupling relocated one slice earlier"):
  - `typedef wf_step_result (*wf_block_exec_fn)(wf_ctx*, const wf_node*)` registered
    per block type via a vtable. **There is ONE engine call-site.**
  - `wf_step_result` is a small **discriminated union the engine switches on**:
    `advanced | pending | failed | looped`, plus only engine-relevant payload — a
    produced-artifact handle + `content_hash`, a `pause_reason` (a **stable tagged
    enum**: `pending_human|panel_degraded|budget_exceeded|panel_unreachable` — never a
    free-form string, so the narrow seam can't silently re-acquire a surface), a
    `reopen` flag, and a `cost` field. This is the only struct the W1 contract test
    pins.
  - **Gates are ordinary block executors** (`gate.roundtable`/`gate.human` register
    the same `wf_block_exec_fn`). There is **no second `wf_gate_run` engine seam**: a
    gate executor internally runs its panel/approval logic, then **maps its result to
    a `wf_step_result`** (APPROVE→`advanced`, REQUEST_CHANGES→`looped`, park→`pending`).
  - **Gate-internal types are NOT frozen here.** `wf_verdict` (verdict enum,
    `blockers[]` incl. `severity`, `schema_version`, `reviewed_content_hash`),
    `wf_panel_spec` (required/eligible, circuit-breaker counts, rotation seed), and the
    W4 signed-approval payload live in the **W4/W5 slice headers** and may grow
    freely — the engine never inspects them, so W5/W6 add fields without amending W1.
- The **validator**: typed handle resolution (each input → exactly one reachable
  upstream producer of the matching type), rejects unbound/type-mismatch,
  two-producers-without-`select`, non-loopback cycles, unreachable nodes, dangling
  control edges, no-terminal, **`gate.roundtable` with `required` < 2** (single-lens
  floor), `optional:true` with `pr_review`.
- **Canonical-form normalize** (lexicographic node-id order + explicit `{kind,to}`
  edge lists + deterministic `<producer_id>.<output>` handle naming — stable under
  cycles) → the content-hash **`version`**.
- `aimee workflow list | show | validate | blocks | new` (read/validate only here).
- Ship the default **`build.yaml`** + a `$AIMEE_HOME/workflows/` loader (defaults +
  deployment override).
- **Files:** new `src/workflow/wf_def.{c,h}` (parse/model), `wf_iface.h` (frozen
  interfaces), `wf_validate.c`, `wf_canonical.c`, `src/cmd_workflow.c`, default
  `workflows/build.yaml`, headers + build wiring (CORE_SRCS/CMake/TEST_CORE_OBJS).
- **Test:** unit on the validator (every reject path) against a **future-complete**
  `build.yaml` exercising all catalog block types; canonical-form byte-stability incl.
  a cyclic graph; a contract test pinning **only the narrow `wf_step_result` +
  `wf_block_exec_fn`** shape (NOT the gate-internal verdict/panel types — those are
  free to evolve in W4/W5).

## Slice W2 — Work-item state machine + store (DB1; gates stubbed)
- Work-item row (`work_item_id` server-minted 128-bit, `repo` normalized +
  `proposal_path` UNIQUE, `state`, `mode`, `workflow_name/version`, `current_stage`,
  `transitioned_by/at`, `last_verdict_id`, `content_hash`, `cum_cost_usd`,
  `override_count`, `pause_reason`, `paused_state`) + immutable `lifecycle_event`
  audit log; DB1 default (`lifecycle.store` switch reserved for DB2).
- The **graph engine**: `advance` drives the pinned workflow graph **entirely through
  the W1 `wf_iface.h` vtable** — it threads the block/gate **call-site, the
  `pending`/pause propagation, the loop/`reopen` handling, and the atomic
  cost-reservation hook** (reserve→reconcile against `cum_cost_usd` in the advance
  txn) once, here. Every block (incl. gates) is invoked via a **registered stub
  executor** that returns `advanced`/APPROVE, so the engine is exercised end-to-end
  before any real executor exists — and W3/W4/W5 add executors **without re-patching
  `wf_engine.c`**.
- Terminal/`rejected`/`abandoned` states, loop-back `max_attempts` accounting, the
  `BEGIN IMMEDIATE` per-work-item lock.
- Front-matter derive/regenerate + reconcile (row wins, `--force`); **migration**
  (existing `State:` → state; missing/unrecognized → `draft` + flag).
- `aimee lifecycle status | list | log`, `advance`.
- **Files:** `src/workflow/wf_engine.c`, `wf_store.c` (DB1 accessor), `wf_migrate.c`,
  extend `src/cmd_workflow.c` / new `src/cmd_lifecycle.c`.
- **Test:** unit on every transition incl. loop-back + `max_attempts` + abandon;
  migration default-to-draft; concurrency lock rejects a second `advance`.

## Slice W3 — Non-gate block executors (author / implement / freeze / pr / merge)
- `author.proposal`/`author.plan` (delegate-driven authoring on the work item),
  `implement` (delegate fan-out), `freeze` (records base SHA = `merge-base` at freeze
  + freeze-commit SHA as `wi/<id>/freeze-base` ref; computes `frozen_diff` +
  `hash(diff base..freeze)`; distinguishes `freeze_invalidated` vs `artifact_changed`),
  `pr.open` (push branch / open forge PR — GitHub first), `merge`.
- Each executor **registers behind the W1 `wf_iface.h` vtable** and reports its
  estimated/actual cost through the **engine-side reservation hook built in W2** — the
  reservation/reconcile logic is not re-implemented per executor.
- **Files:** `src/workflow/wf_blocks_author.c`, `wf_blocks_impl.c`,
  `wf_blocks_git.c` (freeze/pr/merge), a forge abstraction `wf_forge_github.c`.
- **Test:** freeze re-open kinds (no-op commit = no round burned; real commit =
  reset); cost reservation under concurrent invocations; fixture-mocked forge.

## Slice W4 — `gate.human` (non-repudiable, autonomy-aware)
Registers a `gate.human` executor behind the W1 `wf_block_exec_fn` vtable and maps
its approval result to a `wf_step_result` (`advanced` on approval, `pending` +
`pause_reason` when parked, `reopen` on hash-staleness) — no engine/interface change.
The signed-approval payload + verify logic live in **W4's own `wf_approval.h`**, not
the W1 seam.
- Approval key (`$AIMEE_HOME/.approval-key`, 0600, delegate has no read access via the
  existing cred-isolation boundary); `aimee lifecycle approve` signs `{actor, gate,
  work_item_id, content_hash, ts}`; engine verifies. Policies `interactive` /
  `pr_review` (forge PR-approval is the trusted signal) / `preauthorized` (signed
  grant + expiry). Content-hash staleness re-opens.
- **Files:** `src/workflow/wf_gate_human.c`, `wf_approval.c` (sign/verify),
  extend `cmd_lifecycle.c` (`approve`).
- **Test:** a delegate cannot mint a valid approval (no key); artifact edit re-opens;
  preauthorized grant honored + expiry.

## Slice W5 — `gate.roundtable` (live wiring **§0-gated**; logic mock-tested now)
- The fail-closed gate: **panel-eligibility predicate** (`tools_enabled` ∨ no-tools
  review mode, `review` role, reachable), **required vs eligible**, **mid-round
  failure** (`panel_degraded` pause, round not burned, `retry-round`), **verdict
  contract** (`schema_version`, `reviewed_content_hash` ≠ lifecycle hash ⇒
  REQUEST_CHANGES, per-delegate malformed circuit-breaker), **advance rule**
  (quorum over APPROVE + floor `max(2,…)`, COMMENT=present-not-approve, high-sev
  blockers gate independently), **"resolved"/carry-forward**, **round cap +
  `gate-override`** (signed, capped), **anti-gaming** (rotation seed + rubber-stamp
  metric), **approval-binds-hash + reset-round-on-hash-change**.
- Registers a `gate.roundtable` executor behind the W1 `wf_block_exec_fn` vtable
  (maps verdict → `wf_step_result`) — no engine change. The `wf_verdict` /
  `wf_panel_spec` types (blocker `severity`, `schema_version`, `reviewed_content_hash`,
  required/eligible, circuit-breaker counts, rotation seed) live in **W5's own
  `wf_verdict.h`** and may grow without touching the W1 contract test.
- **§0:** the *logic* is unit/fixture-tested against a deterministic mock panel now;
  the *live* call into the ensemble engine is wired when `roundtable-panel-composition`
  ships (it provides per-participant personas + a no-tools review mode for codex). The
  **codex-as-reviewer (no-tools mode) tests are deferred to §0 landing** — not claimed
  in W5; W5 tests the contract/eligibility/quorum logic model-agnostically via the mock.
- **Files:** `src/workflow/wf_gate_roundtable.c`, `wf_verdict.c` (contract
  validate/score), integration shim to `delegate_ensemble`.
- **Test:** canned verdicts (APPROVE/mixed/malformed/perpetual-contrarian/COMMENT-only
  -required); tampered-artifact ⇒ REQUEST_CHANGES; circuit-breaker; all-down + below-
  minimum fail-closed; required panelist mid-round drop; post-APPROVE edit re-opens
  with a fresh round budget.

## Slice W6 — Autonomy driver + trigger + cost/rate integration
- `mode: autonomous` driver: auto-advance machine gates on APPROVE + `preauthorized`/
  `optional` human gates; park `pending_human` otherwise; **`gate-override` is a human
  approval → autonomous can't self-override**. Enumerated pause/resume per
  `pause_reason` (`pending_human`/`panel_degraded` park; `budget_exceeded` →
  `resume --budget-bump`; `panel_unreachable` auto-retry+backoff then park).
- The autonomous-proposal-loop refactored to **call** the engine. The **enforced,
  non-bypassable trigger** (guardrail hook refuses a matching push that skipped the
  workflow; override only via recorded `gate-override`). Two-level cost caps + global
  concurrent-work-item rate limit. `resume`, `retry-round`, `gate-override` verbs.
- **Files:** `src/workflow/wf_autonomy.c`, trigger hook in the guardrails layer,
  refactor the autonomous-proposal-loop caller, extend `cmd_lifecycle.c`.
- **Test:** autonomous run completes with preauthorized gates + parks (cannot
  self-override) at a stuck roundtable + hard-stops a non-preauthorized human gate;
  trigger blocks a bypass push; budget pause + resume.

## Slice W7 — Web visual composer (last; depends on stable /v1 + catalog)
- `/v1` workflow routes (list/show/validate/save/blocks + work-item run-state);
  node-graph editor in `aimee/frontend/` (palette = block catalog, drag/wire/param,
  server-side validate inline, **canonical-form normalize on save** + optimistic-lock
  on `version`, **live-run view renders the work item's pinned version**).
- v1 = single-editor (no realtime collab).
- **Files:** `src/server/server_http_routes` (+ openapi + gen-cli-v1-routes), KB/server
  `/v1` handlers, `aimee/frontend/` editor components.
- **Test:** CLI↔web round-trip byte-stable (incl. cyclic graph); save rejects on
  version mismatch; live view highlights the pinned-version current stage.

## Cross-cutting
- **Sequencing:** W1 → W2 → W3 → W4 → W5(logic) → W6 → W7; W5's **live** wiring waits
  on §0 (`roundtable-panel-composition`) — land that dependency before/around W5.
- **Each slice:** clang-format-19; `make -C src ../aimee-server` + `../aimee-kb` +
  `unit-tests` + lint + build-integrity green before PR; **per-slice roundtable**;
  testing → main. No AI attribution.
- **New TU build wiring:** every new `src/workflow/*.c` added to CORE_SRCS + CMake +
  the hand-curated TEST_CORE_OBJS lists (known gotcha).
- **Backward-compat:** workflows are opt-in (the §10 trigger); existing proposals
  migrate to `draft` + flag; no behavior change for repos that don't enable the
  lifecycle.
- **Out of scope (separate proposals):** `roundtable-panel-composition` itself (the
  §0 dependency — its own proposal/plan); the curator/embedder tracks.
