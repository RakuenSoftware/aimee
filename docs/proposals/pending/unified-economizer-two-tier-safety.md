# Proposal: Unified economizer with a two-tier safety model

- **State:** proposed — design (pending review). Consolidate aimee's **six overlapping
  context-reduction systems** into a **single economizer** with an explicit **two-tier**
  safety model: a **safe, default-ON** baseline every user gets, and an **aggressive,
  default-OFF** tier the user opts into. The promotion gate between tiers is **recovery
  cost** — a lever is default-on-safe only when what it removes is losslessly recoverable
  via one handle *and* the condensed view rarely forces the agent to pay it back.
- **Thesis:** aimee reduces context in **six independent places** — a hard 32 KB
  tool-output cap, `compact.c`, `reduce.compress`, `reduce.command_filter`,
  `reduce.history_fold`, `ingress_compress`+`code_span`, and `gateway_mutate`. Four of
  them reduce **tool output**, with **three different recovery mechanisms** (none,
  Coordinate Closet, spill, `code_span_get`) and **inconsistent defaults** — some on, some
  off. This fragmentation has two costs. (1) **Every new lever measures "marginal"**
  because the others already ate the bytes before it ran — the fold's 84.5%-offline vs
  net-marginal-live gap is an artifact of double-coverage, not a weak lever. (2) **The
  *on-by-default* reducers are the *lossy* ones** (the 32 KB hard cap truncates
  destructively; `compact.c` summarizes) while the lossless-recoverable ones
  (`command_filter`, `compress`) are off. The safe choice is off; the unsafe one is on.
  This proposal unifies the systems and inverts that: **make the safe, lossless levers the
  default, and gate the aggressive ones behind an explicit opt-in and a measured
  recovery-cost bar.**

## §0 The current fragmentation (verified)

| System | Reduces | Default | Recovery | Lossy? |
| --- | --- | --- | --- | --- |
| Hard tool cap (`AGENT_TOOL_OUTPUT_RAW_MAX` 32 KB / `tool_output_max_bytes`) | tool output | **on** | none | **yes (truncated)** |
| `compact.c` (`compact_body`, head/tail summary) | tool output + history | **on** | none | **yes** |
| `reduce.compress` (body compression) | tool output | off | Coordinate Closet | recoverable |
| `reduce.command_filter` (RTK) | tool output | off | spill | recoverable |
| `reduce.history_fold` | old history | off | `fold_recall` | recoverable |
| `ingress_compress` + `code_span` | inbound `/v1` request | **on** | `code_span_get` | recoverable |
| `gateway_mutate` | **live** `/v1` request | off | restore | live-risk |

Four tool-output reducers; three recovery contracts; on-by-default = the lossy ones.

## §1 The unifying principle — recovery cost is the safety axis

"Safe default + aggressive opt-in" reduces to one variable: **how much does a reduction
force the agent to pay it back?**

- **Default-on-safe** ⇔ removed content is **losslessly recoverable via one handle** AND
  the condensed view **rarely forces recovery** (it keeps what the agent usually needs).
- **Aggressive / default-off** ⇔ lossy without cheap recovery, OR mutates live client
  traffic, OR has **high recovery cost** — the agent keeps paging content back in, which
  *erodes or inverts the saving* (this is exactly the net-marginal problem, named).

**Two distinct promotion gates (they are not the same):**
- **Deterministic gate** — for levers whose recovery cost is **low by construction** (they
  provably keep the signal the agent needs and drop only noise). Passing = lossless-on-
  demand + fail-open + a *no-over-reduction* audit (never elides a failure/error/diagnostic
  the agent would need) proven with a **deterministic** test, no LLM. `command_filter` is
  in this class, which is what lets **P1 default it on without waiting on P4**.
- **Measured recovery-cost gate** — for levers whose recovery cost is **uncertain**
  (`history_fold`, `compress`): they only earn promotion to the safe tier once P4's live
  telemetry shows their **net-of-recovery** savings clear a numeric bar. Default-off until
  then. This is the gate the fold's "net-marginal" number failed, honestly.

A worked example already in hand: the `command_filter` test-runner family keeps *which*
test failed (the `--- FAIL:` marker) but can elide the failure's **detail message** (the
assertion text) if it isn't in the head/tail window — measured deterministically on a real
`go test` run (85% shrink, but `TestFailAlpha`'s `expected 5 got 4` was elided). The detail
*is* in the spill (lossless), but hiding the "why" forces a recovery round-trip to fix the
test → **high recovery cost → not yet default-on-safe.** Keep the detail block by each
failure → recovery becomes rare → *now* safe to default on. Same lever, moved across the
tier boundary by a small correctness fix. That is the promotion pipeline in miniature.

## §2 The target architecture

A single `economizer` subsystem (extends `context_reduce`) with:

1. **One recovery handle.** Retire the three separate recovery paths (spill, Coordinate
   Closet, `code_span_get`) behind **one addressable "reduced → recoverable" contract**.
   Losslessness becomes a **system invariant**, not a per-lever promise — which is what
   *lets* a lever be safe to default on.
2. **Two explicit tiers.** `economizer.enabled` (safe baseline, **default-ON**) and
   `economizer.aggressive` (**default-OFF** opt-in). Retire the scattered flags; map the
   legacy keys (`reduce.*`, `ingress_compress`, `tool_output_max_bytes`) onto the new
   system for back-compat.
3. **One pipeline order** so the levers stop fighting / double-reducing: *recognize →
   condense tool output (lossless) → fold old history → place the cache boundary.* Today
   they run at unrelated seams with no coordination, which is why they double-cover.
4. **One ledger.** The baseline-vs-reduced accounting (the `usage_kind='avoided'` rows)
   becomes the single source of truth, counted **once** across the pipeline — so the
   "incremental win" number is finally honest instead of double-counted per system.
5. **Recovery-cost telemetry as a first-class signal.** Count how often reduced content is
   paged back in (spill / `fold_recall` / `code_span_get` reads). That number **is** the
   default-on gate and the aggressiveness governor — it attacks the net-marginal problem
   with data instead of intuition.

## §2.2 The recovery contract (what makes "lossless-on-demand" true)

The one recovery handle is only meaningful if its failure and lifetime semantics are pinned
— so this is specified *before* any default-on flip:

- **Store + scope.** Per-run spill store, mode `0700`, **opaque random refs** (not
  enumerable). Never shared across runs/users — no cross-run leak.
- **Lifetime + eviction.** Available for the run **plus the immediately following turn**;
  a per-run **max-total-bytes** cap with **oldest-first (LRU) eviction**. A retrieval
  against an evicted/expired ref returns an explicit `spill expired` marker, never garbage.
- **Durability = atomic write.** "Durably spilled" is a *verifiable* predicate, not
  aspirational: write to a temp file → `fsync` → atomic `rename` to the ref path → `fsync`
  the directory; the success ack is returned **only after the rename is durable**. A crash
  mid-write leaves a temp file that is never promoted, so a partial file can never be read
  as "spill succeeded."
- **Spill-write failure ⇒ no condensation, no dangling pointer.** If the full content
  cannot be durably spilled, the lever **does not condense**: it passes the raw output
  through (bounded by the §5 ceiling) with an explicit `TRUNCATED — N bytes dropped, full
  output unavailable` marker if the ceiling clips it. Losslessness is claimed **only** when
  the spill succeeded — there is never a pointer to content that was not written.
- **Eviction/read race.** A retrieval takes an in-use refcount (flock the entry) before
  opening; eviction observes the in-use state and skips a spill with a pending read — so a
  read never returns a half-evicted partial (it returns the content, or a clean
  `spill expired`, never a truncation window).
- **Pointer preservation is a downstream invariant.** A recovery pointer, once written into
  a condensed body, is **opaque pass-through** for every later step (fold, compress,
  gateway_mutate): no downstream lossy step may summarise or drop it. A step that cannot
  preserve a pointer must **refuse to run** on that content — otherwise the recovery handle
  is lost and the losslessness invariant breaks silently.
- **Pointer schema** (carried inline in the condensed body): `{ ref, family,
  confidence∈[0,1], raw_bytes, pre_cap_bytes }`. `confidence` is the recognizer's
  calibrated score; `raw_bytes` lets an agent see how much was elided; `pre_cap_bytes`
  surfaces near-ceiling tool calls as a misuse signal.
- **Agent recovery rule** (documented for the tool): retrieve the raw when the agent needs
  an elided detail, OR proactively when `confidence < 0.7` or `family == unknown` (a
  possible mis-recognition) — via the single `tool_output_get(ref)` retrieval, itself
  isolation-wrapped (bounded returned bytes).

## §3 The tiering (current state, not aspirational)

**Safe tier — becomes default-ON in P1 (passes the deterministic gate):**
- `command_filter` (RTK) — deterministic, spill-recoverable, fail-open, **after the
  failure-detail fix** — and it **fronts** the tool-output cap (see §4 P1), turning today's
  destructive truncation into lossless-recoverable condensation.
- Cache-boundary placement (`ingress_cache_placement`) — a pure win, no information loss.

**Aggressive tier — default-OFF:**
- `gateway_mutate` — live client traffic; stays behind its circuit breaker.
- High-ratio ingress compression.

**P5 candidates — default-OFF until *measured* (do not read as safe-tier members today):**
- `reduce.compress` (body compression) — promote only if P4's net-of-recovery bar is met.
- `reduce.history_fold` — promote only if net-of-recovery + no-lost-signal clear the bar;
  otherwise it stays honestly aggressive.

## §4 Phased plan (each phase roundtable-gated + shippable)

- **P1 — the one place the current default is unsafe** (justified by the *deterministic*
  gate, §1 — not P4). (a) Fix the `command_filter` failure-detail gap: keep the diagnostic/
  failure detail *block* adjacent to each failure marker, not just the marker line (a
  context window around each fail-signal). Land a **deterministic no-over-reduction test**:
  on a corpus of real test/build failures, every failure/error/diagnostic present in the
  raw output is present in the *condensed* output (not just recoverable in the spill). (b)
  Route the tool-output seam through `command_filter` *before* the cap: a **recognized**
  family condenses losslessly + spills; **unrecognized** output keeps a **configurable hard
  ceiling** (raised from 32 KB, e.g. low-MB) but truncation now carries a **recovery
  pointer to the spilled full blob** — never a silent destructive truncation. The
  recognizer confidence + family is surfaced on the pointer so an agent that suspects a
  mis-recognition can self-recover the raw output. (c) **Default `command_filter` ON.** Net:
  today's on-by-default *lossy* truncation becomes lossless-recoverable.
- **P2 — one recovery handle.** A single `tool_output_get`-style retrieval contract that
  the spill, `fold_recall`, and `code_span_get` all resolve through; losslessness as an
  invariant with a conformance test.
- **P3 — two-tier config.** `economizer.enabled` / `economizer.aggressive`; legacy-flag
  mapping; the web Settings surface reflects the two tiers.
- **P4 — recovery-cost telemetry + the promotion gate.** Instrument page-backs and publish
  the **net** savings, not the gross. Worked definition:
  `net_tokens = avoided_baseline_tokens − (page_backs × mean_recovery_tokens)` and
  `net_seconds = reduction_compute_s + spill_write_s − (page_backs × mean_recovery_latency_s)`
  — including the **spill cost** itself (write compute + `fsync` latency), so a lever that
  needs huge durable spills isn't promoted on condensation arithmetic alone. A lever is
  net-positive only when **both** are > 0 (a high-frequency-but-cheap lookup fails on
  `net_seconds` even if `net_tokens` survives). Define the numeric bar once (net-positive
  with margin, over a window, with a no-lost-signal audit) and apply it uniformly to every
  P5 promotion.
- **P5 — promote what proves safe.** Use P4's data to move `compress` / `history_fold`
  into the safe tier if (and only if) their measured net-of-recovery savings clear the bar;
  otherwise they stay aggressive, honestly.

## §5 Safety contract
- Every safe-tier lever is **lossless-on-demand** (one recovery handle) + **fail-open**
  (any error → original) + **no live-client mutation**.
- **Fail-open is still capped.** The hard cap is *raised + recovery-pointed*, not removed:
  every fail-open path is bounded by a **configurable last-resort ceiling** (default
  **2 MB** — well above typical tool output, below a DoS blast radius) so a misbehaving/
  adversarial tool emitting a multi-hundred-MB blob can never flow unbounded into context.
  Steady-state is lossless with spill; when the spill *succeeded* the ceiling clip carries a
  recovery pointer, and when it did **not** the clip carries an explicit "full output
  unavailable" marker (§2.2) — never a dangling pointer.
- The default-on flip is gated by the **deterministic** gate (safe-by-construction levers,
  proven with a no-over-reduction test) OR the **measured net-of-recovery** gate (P5
  candidates) — never a headline offline number, and never before the relevant gate.
- **Back-compat is explicit + symmetric:** legacy keys are read once on upgrade and
  translated with a one-time migration notice. An explicit `reduce.*=false` **stays off**
  (opt-out honored); an explicit `reduce.compress=true`/`history_fold=true` set by a power
  user translates to explicit `economizer.aggressive=true` (opt-*in* honored, never silently
  downgraded). Only a *never-set* lever inherits the new default.
- Aggressive tier stays off until its own gate is met; no silent promotion.

## §5.1 Pipeline-order rationale (per step)
The single order — recognize → condense tool output → fold old history → place cache
boundary — is not arbitrary; each step's properties differ and the order avoids
double-reduction and cache thrash:

| Step | Content-preserving? | Fail-open scope | Streaming/retry | Prefix-cache effect |
| --- | --- | --- | --- | --- |
| Recognize+condense tool output | lossless (spill) | per tool-result | applies to the completed result, not mid-stream | appends near the tail (below the cached prefix) |
| Fold old history | lossy→recoverable | per request | never touches an in-flight retry's pristine copy | **reshapes the prefix — must be freeze-guarded** |
| Place cache boundary | preserving | per request | n/a | the point of it |

`freeze_guard` is promoted from a fold-specific guard to a **pipeline-wide precondition
API** with one enforcement point in the pipeline runner —
`pipeline_step_can_run(step, predicted_cache_delta)` — that every prefix-reshaping step
(fold today, any future step) must satisfy: it runs only when the cache-read savings beat
the one-time cache-write churn, so the pipeline never trades a prefill saving for a cache
miss. Tool-output condensation appends **below** the cache boundary (near the tail), so it
does not invalidate the cached prefix; only prefix-reshaping steps are gated. The
cache-boundary placement step is a no-op when no prefix reshaping ran (it is only a "pure
win" in conjunction with a step that moved the boundary).

## §6 Non-goals
- No new reduction *algorithm* — this is consolidation + tiering + a recovery invariant.
- No change to what the agent may do; only how much of the past/tool-output it sees first.
- Not removing the aggressive levers — making their on/off an honest, measured choice.

## §7 Open items (for roundtable)
1. Back-compat: how aggressively to retire the legacy flags vs alias them indefinitely
   (the §5 "explicit-false-stays-off" rule is fixed; the question is the alias horizon).
2. Whether the unified handle reuses the existing `code_span`/memory addressing or a
   dedicated per-run spill ref (both satisfy §2.2 — this is an implementation choice).
3. The exact numeric promotion bar + window length for P4 (the *shape* — net-positive in
   tokens AND seconds with a no-lost-signal audit — is fixed; the thresholds are TBD).
4. Whether 2 MB is the right last-resort ceiling default across deployment tiers.
