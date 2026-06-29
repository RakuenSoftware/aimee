# Deterministic context folding

Sustain long agent sessions inside a fixed context window by **deterministically
folding old conversation turns** — no LLM summarization call — while conserving the
exact identifiers an agent needs, and keeping the provider prompt cache warm.

**Status:** shipped on `testing`, **default-off** end to end. Each layer is gated by
its own config flag; with all flags off, behaviour is byte-identical to pre-fold.

This guide is the operator/developer reference. The design rationale is in
`docs/proposals/done/deterministic-context-folding.md`; the pipeline-order contract
is in `docs/dev/fold-pipeline-order.md`.

## What it does

On aimee's **delegate** request path (`posix/agent_runtime.c`, Anthropic shape),
once a conversation grows past `retained_msgs` (default 8) trailing messages and at
least `min_fold_msgs` (default 4) older turns are eligible, a contiguous prefix of
older turns is replaced by a single synthetic summary:

- **Rolling fold (§1)** — one skeleton line per turn / tool call (`assistant: …`,
  `  $ tool …`, `    → result (N bytes)`), plus a budgeted reasoning excerpt.
- **Coordinate Closet (§2)** — the exact identifiers from the folded region (uuids,
  commit shas, paths, digit-bearing `key=value`, issue/PR refs, `handle:`/`memory:`
  tokens) are conserved verbatim in a labelled block so truncation can never
  amputate them. Secrets are redacted; user-pasted content is *quarantined* —
  conserved in a separate lane, rendered with an `(untrusted)` marker under a
  divider, and never allowed to mint an agent-trusted label (a prompt-injection
  guard).
- **Fold-freeze (§3)** — the fold boundary is *pinned* across turns, so the folded
  prefix stays byte-identical turn-to-turn and the provider cache stays warm; the
  boundary only advances at an "epoch" when the un-folded tail outgrows its cap.
- **Register grammar (§6)** — folded assistant lines are annotated with their
  register (verdict / hazard / executing / blocked / in-progress) so the summary
  preserves which turns were settled conclusions.
- **Fold recall (§4)** — if a later turn re-references a folded-away coordinate, a
  recall hint is surfaced so the body can be paged back in on demand via the
  existing `code_span_get` / `memory_get` tools.

Two more building blocks ship as tested libraries (storage binding is future work,
see [Limitations](#limitations-and-deferred-work)):

- **Sealed episodes (§5)** — a replayable checkpoint (conclusion + file inventory)
  with a file-touch auto-recall predicate (`src/episode_seal.{c,h}`).
- **Task rail (§8)** — a portable plan FSM serialisable outside the prompt
  (`src/task_rail.{c,h}`).

A seventh piece underpins the rest:

- **Per-model budget resolver (§7)** — `src/fold_budget.{c,h}`, a *pure* function
  that turns a model id into the fold's token budgets. It has **no user-facing
  config surface** (the operator knobs below are message-count/byte based); it is
  listed here only so the §1–§8 map is complete and so operators understand fold
  output may legitimately differ across model identities.

Everything is **deterministic** — no LLM-summarisation call, no wall-clock input,
no randomness — so identical input yields byte-identical output, which is what makes
the freeze and the cache benefit possible. (Model *identity* is an input via §7, so
switching models is treated as a prefix change — see the freeze invariant.)

## Enabling it

All knobs live under the `fold` and `compact.coord_closet` config sections
(`~/.config/aimee/aimee.yaml`). **Boolean** `*_enabled` flags default **off**.
**Numeric** keys use **0 as a sentinel** that resolves to a built-in module default:
`retained_msgs`→8, `min_fold_msgs`→4, `excerpt_bytes`→160, `budget_bytes`→2048,
`max_ratio_pct`→100, `tail_cap_msgs`→24, `ttl_turns`→4. The snippet below is an
**all-on reference** — see the staged rollout after it for the recommended order.

```yaml
fold:
  enabled: true            # §1 turn out the rolling fold on the delegate path
  retained_msgs: 8         # trailing messages kept at full fidelity (0 = default 8)
  min_fold_msgs: 4         # fold only if at least this many messages would fold
  excerpt_bytes: 160       # per-message reasoning excerpt kept in the skeleton
  register_enabled: true   # §6 annotate folded assistant lines with their register
  freeze:
    enabled: true          # §3 pin the boundary across turns (cache-warm)
    tail_cap_msgs: 24       # advance the boundary when the un-folded tail exceeds this
  recall:
    enabled: true          # §4 emit recall hints on re-touch of a folded coordinate
    ttl_turns: 4            # don't re-surface the same coordinate within N turns

compact:
  coord_closet:
    enabled: true          # §2 conserve verbatim identifiers (also used by the fold)
    budget_bytes: 2048      # hard cap for the conserved block (0 = default)
    max_ratio_pct: 100      # closet <= raw_len * pct/100 (0 = default 100)
    denylist: "corpsecret"  # extra secret substrings (comma/space separated)
```

Recommended order to turn on (each is independently safe):
1. `compact.coord_closet.enabled` — conserve identifiers in tool-result compaction
   (works even without the transcript fold).
2. `fold.enabled` (+ `register_enabled`) — the transcript fold itself.
3. `fold.freeze.enabled` — once folding is on, freeze keeps the cache warm.
4. `fold.recall.enabled` — re-touch hints.

## How it behaves (invariants)

- **No silent coordinate loss.** Conserved identifiers are rendered verbatim when
  they fit; a nominated identifier that cannot fit the closet budget is explicitly
  signalled (`COORD_EVICT_FAIL`) and rendered as a `(partial — … omitted)` marker —
  never silently dropped.
- **Atomic tool pairs.** The fold boundary is always a *clean user turn*, so a
  `tool_use` and its `tool_result` are never split, and the retained tail always
  begins with a user message (valid Anthropic alternation).
- **Non-destructive.** The raw transcript is never mutated; the fold produces a new
  view used only to build the outbound request.
- **Cache stays warm.** With freeze on, the folded prefix is re-emitted byte-for-byte
  while it is pinned; a mid-run prefix mutation (e.g. compaction) is detected by a
  digest and forces a clean epoch rather than a false cache claim. A model-identity
  change (different §7 budget) likewise shifts the prefix, so it is handled the same
  way — a fresh epoch, not a stale cache-warm claim.
- **Secrets are redacted before render** by the implemented detectors —
  `ghp_`/`sk-`/`AKIA…`/JWT/PEM token shapes, credential-bearing paths, and sensitive
  label names — plus any substrings added via `compact.coord_closet.denylist`. The
  detector set is extensible, not exhaustive: it is a strong filter, not an absolute
  guarantee against every possible secret shape.

## Observability

With `AIMEE_LOG`/debug logging, the fold emits `fold folded=N retained=M
reused_boundary=B epochs=E` per folding turn. `coord_closet partial (budget)` warns
on closet eviction. `fold_result_t.reused_boundary` reports cache-warm reuse.

## Limitations and deferred work

The fold is **default-off** and these are the honest gaps (tracked in the proposal
close-out):

- **Delegate path only.** The fold targets aimee's own agent loop, which already
  owns the `payload_rewrite` cache machinery. The ingress proxy path
  (`server/anthropic_http.c`, external clients) is not folded yet.
- **`payload_rewrite` span registry (§3 substrate).** The cache benefit here comes
  directly from emitting byte-identical prefixes; integrating the fold span into
  `payload_rewrite`'s single prefix hash (and the single-owner CI lint) is a
  follow-up that converges with `ingress-compression-and-cache-alignment`.
- **Episode/task-rail storage binding.** §5 and §8 ship as tested modules; DB1
  checkpoint persistence for the rail and DB2 `unit_type` storage + cross-session
  file-touch recall for sealed episodes are the storage-binding follow-up.
- **Within-run freeze.** Freeze pins the boundary across turns *within a run*;
  cross-session (DB-persisted) freeze is future work.
- **Recall is hint-only.** Re-touch surfaces a hint pointing at the on-demand
  recovery tools; automatic inline body fetch is deferred.

## Default-on candidacy

Folding is a token/cost optimisation with an agentic-recovery cost (a folded body
must be re-fetched if needed). Per aimee's off-reasons discipline, default-on is
gated on the integration-test results (token reduction, cache-read share, and
0-lost-coordinate fidelity on a captured long session). See the proposal close-out
for the assessment.
