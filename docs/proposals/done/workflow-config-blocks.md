# Design: config-extensible workflow blocks (+ CI/merge safety blocks)

- **State:** design converged (2 rounds; reviewer READY, architect design-convincing — code-verification items deferred to the implementation roundtable)
- **Builds on:** the merged workflow engine (`aimee-dev-lifecycle-workflow.md`, PR #288).
- **Goal:** let users **define their own composable blocks via config** (no recompile),
  type-checked by the same validator and run by a generic executor; and add three
  built-in **safety blocks** (`gate.ci`, `check.mergeable`, an idempotent `merge`)
  so a workflow can't merge red, conflicted, or already-merged work.

## Motivation

Today the block catalog is hard-coded (C `CATALOG[]` + the `WFE_BLK_*` enum +
C-registered executors). Users can compose *workflows* from the built-in blocks
but cannot add a new *block* (e.g. "run my linter", "post to Slack", "deploy")
without editing the engine. And the engine has no built-in guard against the
exact failure that just bit us — trying to merge a PR whose CI is red.

## Part A — config-extensible blocks

### Block registry (built-ins + config, merged at load)
The catalog becomes a **runtime registry** = the static built-ins **plus** custom
blocks loaded from `$AIMEE_HOME/workflows/blocks.yaml`:

```yaml
blocks:
  - name: lint                 # unique; must not shadow a built-in
    consumes: branch           # one of the closed artifact types (or "none")
    produces: branch
    executor: command          # command | delegate
    command: ["make", "lint"]  # command executor: argv run in the work-item repo
  - name: security-scan
    consumes: branch
    produces: branch           # scan-and-autofix transform; the GATE verdict comes
    executor: delegate         # from gate.roundtable, NOT this block
    persona: security
    prompt: "Scan the branch diff for vulnerabilities and push fixes onto the branch."
```

Rules (validated at load; load fails closed on any violation):
- `name` unique + not a built-in (shadowing a built-in is rejected).
- `consumes` ∈ the closed artifact enum **or `none`**; `produces` ∈ **`branch` or
  `none` only**. Custom blocks **cannot mint `verdict`, `approval`, or `pr`** —
  those are *trust-bearing* artifacts produced only by the built-in gates
  (the panel-scored verdict), `gate.human` (a signed approval), and `pr.open`.
  A custom block transforms the branch (`branch→branch`, e.g. lint/format/deploy)
  or is a side-effecting sink (`…→none`, e.g. notify). `consumes:none` (a source)
  and `produces:none` (a sink) are both allowed and symmetric.
- `executor ∈ {command, delegate}`; command blocks need `command` (argv), delegate
  blocks need `persona` + `prompt`.
The graph validator's **algorithm is unchanged**; it now resolves block types
against the **runtime registry** (built-ins ∪ custom) instead of the static array,
so a workflow using a custom block is type-checked exactly like one using built-ins
(unbound/typed-mismatch still rejected).

### One generic executor, keyed by `executor`
The frozen `wfe_iface.h` seam is untouched: a custom block is a single block type
`WFE_BLK_CUSTOM` whose node carries the resolved spec. A generic
`exec_custom(ctx, node)`:
- **command** — **opt-in only**: refused unless `lifecycle.allow_command_blocks:
  true` in config (default false), because it runs operator-authored argv. Runs
  the `command` **argv directly (never via a shell — no string interpolation)**
  through `safe_exec_capture`, with the working directory pinned to the resolved
  work-item repo. Non-zero exit → `WFE_STEP_FAILED`; success → the declared
  artifact (`branch` → the branch head SHA; `none` → a sink, no artifact). Trust:
  a `command` block is exactly as privileged as the operator's own shell; the
  block registry is **operator-owned config** (same trust tier as `aimee.yaml`),
  hence the explicit opt-in.
  - **Operational contract** (a `command` block can't hang or flood the engine):
    minimal/clean environment (PATH + a few safe vars, never the approval key or
    other secrets), a wall-clock timeout (`lifecycle.command_timeout_ms`, default
    ~120s → timeout ⇒ `WFE_STEP_FAILED`), and a captured-output cap (the existing
    `safe_exec_capture` max-bytes bound).
  - **Threat model:** the concern is not the operator (who can run anything) but a
    malicious workflow author or a tampered `blocks.yaml`. The registry is loaded
    only if it is operator-owned and not world/group-writable (the approval-key
    perm check: refuse if `mode & 0022`), so a workflow author cannot introduce a
    `command` block without write access to operator config AND the global opt-in.
- **delegate** — dispatch the configured persona/prompt against the branch
  (integration-gated like the other delegate-driven blocks); the produced artifact
  is the (mutated) `branch` head, or `none`. A delegate custom block does **not**
  produce a `verdict` — gating is the job of `gate.roundtable` with its verdict
  contract.

No new entry in the frozen seam; the executor vtable gains exactly one slot
(`WFE_BLK_CUSTOM`); per-custom behavior is data, not code.

### CLI + web
`aimee workflow blocks` lists built-ins **and** custom blocks (marked `custom`);
the W7 web composer renders both in the palette automatically.

## Part B — three built-in safety blocks (need real forge/CI calls)

These can't be pure config (they call the forge), so they ship as built-ins.

**`gate.ci`** (consumes `pr` → `verdict`): polls the PR's combined CI status,
mapping **every** state fail-closed (nothing but an explicit all-green advances):
| forge CI state | gate result |
| --- | --- |
| all checks success | ADVANCE (APPROVE) |
| any failure / error / cancelled / timed_out | LOOP (REQUEST_CHANGES) |
| pending / queued / in_progress | PARK (re-poll later) |
| no checks / unknown / forge unreachable | PARK (never advance) |
Polling is **bounded**: the gate parks (it does not busy-loop); each re-poll is a
fresh advance, and the engine's per-stage `max_attempts` + a `gate.ci.poll_timeout`
cap the wait, then park `pending_human` so a human decides — never an indefinite
spin.

**`check.mergeable`** (consumes `pr` → `verdict`): no merge conflict against base →
ADVANCE; conflict → LOOP (back to implement); `UNKNOWN` mergeability (forge still
computing) → PARK + re-check.

**`merge` hardened (idempotent + race-safe)**: the merge act is the single source
of truth. It (1) reads the PR state; if **already `MERGED`** → no-op success
(never re-push/re-merge); (2) else issues the forge merge, and treats the forge's
own *"already merged"/"not mergeable"* responses as authoritative — an
already-merged race resolves to success, a not-mergeable/lost-race resolves to
LOOP, never a forced or duplicate merge. Detection is not a separate
check-then-act window: the merge command itself is the atomic decision.

The default `build.yaml` gains `check.mergeable` then `gate.ci` between the code
PR's human pass and `merge`, so the base workflow cannot merge conflicted, red, or
already-merged work.

## Validation / tests
- Registry loader: reject duplicate/shadowing names, unknown artifact types, bad
  executor kinds, missing command/persona; a valid custom block round-trips.
- A workflow composed with a custom block type-checks (and a type-mismatched one
  is rejected) by the unchanged validator.
- `exec_custom`: command non-zero → FAILED; command success → declared artifact.
- `gate.ci` / `check.mergeable` decision mapping (mock forge): pass→advance,
  fail→loop, pending→park, unknown→park; `merge` on an already-merged PR → no-op.

## Out of scope
- Custom **artifact types** (the artifact enum stays closed; custom blocks reuse
  it). - Custom blocks that need new C capabilities beyond command/delegate.
- The live `gate.ci`/forge calls are integration-gated (mock-tested here), like
  the existing delegate/forge executors.

## Risks
- A `command` executor runs operator-authored argv (no shell, argv-only) — same
  trust as the operator's shell; gated behind `lifecycle.allow_command_blocks`
  (default false) and run only in the work-item repo. The block registry is
  operator-owned config (same trust as `aimee.yaml`).
- Custom blocks cannot mint trust-bearing artifacts (`verdict`/`approval`/`pr`) —
  enforced at registry load, so a custom block can never impersonate a gate.
- Config drift vs built-ins: a custom block shadowing a built-in name is rejected
  at load (fail closed).
