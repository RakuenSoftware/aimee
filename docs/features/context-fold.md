# Deterministic context folding

Sustain long agent sessions inside a fixed context window by **deterministically
folding old conversation turns** (no LLM summarization call) while conserving the
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

- **Rolling fold (§1)**: one skeleton line per turn / tool call (`assistant: …`,
  `  $ tool …`, `    → result (N bytes)`), plus a budgeted reasoning excerpt.
- **Coordinate Closet (§2)**: the exact identifiers from the folded region (uuids,
  commit shas, paths, digit-bearing `key=value`, issue/PR refs, `handle:`/`memory:`
  tokens) are conserved verbatim in a labelled block so truncation can never
  amputate them. Secrets are redacted; user-pasted content is *quarantined*,
  conserved in a separate lane, rendered with an `(untrusted)` marker under a
  divider, and never allowed to mint an agent-trusted label (a prompt-injection
  guard).
- **Fold-freeze (§3)**: the fold boundary is *pinned* across turns, so the folded
  prefix stays byte-identical turn-to-turn and the provider cache stays warm; the
  boundary only advances at an "epoch" when the un-folded tail outgrows its cap.
- **Register grammar (§6)**: folded assistant lines are annotated with their
  register (verdict / hazard / executing / blocked / in-progress) so the summary
  preserves which turns were settled conclusions.
- **Fold recall (§4)**: if a later turn re-references a folded-away coordinate, a
  recall hint is surfaced so the body can be paged back in on demand via the
  existing `code_span_get` / `memory_get` tools.

Two more building blocks ship as tested libraries (storage binding is future work,
see [Limitations](#limitations-and-deferred-work)):

- **Sealed episodes (§5)**: a replayable checkpoint (conclusion + file inventory)
  with a file-touch auto-recall predicate (`src/episode_seal.{c,h}`).
- **Task rail (§8)**: a portable plan FSM serialisable outside the prompt
  (`src/task_rail.{c,h}`).

A seventh piece underpins the rest:

- **Per-model budget resolver (§7)**: `src/fold_budget.{c,h}`, a *pure* function
  that turns a model id into the fold's token budgets. It has **no user-facing
  config surface** (the operator knobs below are message-count/byte based); it is
  listed here only so the §1–§8 map is complete and so operators understand fold
  output may legitimately differ across model identities.

Everything is **deterministic** (no LLM-summarisation call, no wall-clock input,
no randomness), so identical input yields byte-identical output, which is what makes
the freeze and the cache benefit possible. (Model *identity* is an input via §7, so
switching models is treated as a prefix change; see the freeze invariant.)

## Enabling it

All knobs live under the `fold` and `compact.coord_closet` config sections
(`~/.config/aimee/aimee.yaml`). **Boolean** `*_enabled` flags default **off**.
**Numeric** keys use **0 as a sentinel** that resolves to a built-in module default:
`retained_msgs`→8, `min_fold_msgs`→4, `excerpt_bytes`→160, `budget_bytes`→2048,
`max_ratio_pct`→100, `tail_cap_msgs`→24, `ttl_turns`→4. The snippet below is an
**all-on reference**; see the staged rollout after it for the recommended order.

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
1. `compact.coord_closet.enabled`: conserve identifiers in tool-result compaction
   (works even without the transcript fold).
2. `fold.enabled` (+ `register_enabled`): the transcript fold itself.
3. `fold.freeze.enabled`: once folding is on, freeze keeps the cache warm.
4. `fold.recall.enabled`: re-touch hints.

## How it behaves (invariants)

- **No silent coordinate loss.** Conserved identifiers are rendered verbatim when
  they fit; a nominated identifier that cannot fit the closet budget is explicitly
  signalled (`COORD_EVICT_FAIL`) and rendered as a `(partial — … omitted)` marker,
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
  way: a fresh epoch, not a stale cache-warm claim.
- **Secrets are redacted before render** by the implemented detectors:
  `ghp_`/`sk-`/`AKIA…`/JWT/PEM token shapes, credential-bearing paths, and sensitive
  label names, plus any substrings added via `compact.coord_closet.denylist`. The
  detector set is extensible, not exhaustive: it is a strong filter, not an absolute
  guarantee against every possible secret shape.

## Observability

With `AIMEE_LOG`/debug logging, the fold emits `fold folded=N retained=M
reused_boundary=B epochs=E` per folding turn. `coord_closet partial (budget)` warns
on closet eviction. `fold_result_t.reused_boundary` reports cache-warm reuse.

## Limitations and deferred work

The fold is **default-off** and these are the known gaps (tracked in the proposal
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

## Freeze cost guardrail and cross-provider cache placement

The freeze pins the fold boundary so the reduced prefix stays byte-identical
turn-to-turn, which keeps the provider prompt cache warm. But establishing a cache
entry costs a one-time **cache-write**, and a boundary that keeps advancing can pay
that write repeatedly, which, on a provider that bills cache writes at a premium,
can make freezing cost *more* than it saves.

The **freeze cost guardrail** (`reduce.freeze_guard`, default **on**; only acts when
the economizer freeze is itself enabled, which is default-off) prices the decision
before pinning a boundary. It compares the *marginal* cost of caching against the
savings from reuse, using the same `token_tracker` rates as the rest of the cost
story:

- **Marginal write cost = the write _premium_**, `cache_write_rate − input_rate`,
  not the full write rate. You pay the input rate to send the prefix on the first
  turn regardless; caching only adds the difference. Providers with free cache
  creation (OpenAI/Responses: `cache_write = 0`) have a premium ≤ 0, so freezing is
  pure upside and is always kept on.
- **Per-reuse saving = `input_rate − cache_read_rate`**, accrued over
  `reduce.freeze_guard_horizon` expected reuses (default **1**, clamped to
  `FREEZE_GUARD_MAX_HORIZON`).
- Freeze is pinned when `horizon × per_reuse_saving ≥ write_premium`; otherwise the
  turn re-derives the boundary without pinning (`reduce_result.freeze_guarded`).

For every model aimee currently prices this keeps freeze **on** even at horizon 1
(Anthropic has a small positive write premium that a single reuse's read discount
already covers; OpenAI has no premium). The exact break-even is re-derived from the
live pricing table by `test_freeze_guard`, so a future price change that would flip
the verdict fails that test rather than silently drifting from this prose. The
guardrail therefore changes no current behavior. Its job is to encode
the correct economics as a tested invariant and to disable freeze automatically only
under *adverse* pricing (a write premium that outweighs the reuse savings), or when
caching offers no read discount at all (`cache_read ≥ input`). Missing pricing
**fails open** (freeze stays on, preserving prior behavior).

### Per-provider cache placement

| Provider | Mechanism | Status |
|----------|-----------|--------|
| Anthropic | `cache_control:{ephemeral}` on the stable system-prompt prefix | Existing (`cache_shaping_enabled`); a frozen-history breakpoint is possible future work but low value while reduction keeps the frozen prefix small |
| OpenAI / Responses | Automatic prefix caching, no explicit marker exists or is needed; the freeze's byte-identical ordering already satisfies the cache-hit criteria, and cache writes are free | No code; advisory only |
| Gemini | `cachedContent` resource, created once per run for the system prompt | Existing; extending it to per-epoch message history is deferred (single per-request resource; recreating it per epoch is expensive for the small post-reduction prefix) |

## Default state

The unified **context economizer** (`reduce.*`) is **default-ON at the delegate
seam**: `reduce.measure`, `reduce.delegate_seam`, `reduce.history_fold`,
`reduce.compress`, and `compact.coord_closet` all default true, and the freeze cost
guardrail (`reduce.freeze_guard`) is on. Concretely, every sub-agent (delegate) turn
now folds old history and compresses oversized tool-result bodies, with the
Coordinate Closet conserving the exact identifiers so the lossy reduction stays
recoverable. `history_fold` auto-excludes the Responses/chatgpt builder at the call
site (its synthetic-turn shape is unverified there); `compress` runs for every
provider.

**Recovery model (what "recoverable" means here).** This is a *lossy* reduction with
**agentic recovery**, not lossless caching. Be precise about the guarantee:
- **Recent context is never touched.** Both levers only reduce messages *before* the
  retained tail; the most recent `retained_msgs` (default 8) are byte-for-byte intact,
  so the agent's working set is always full. `retained_msgs` follows the numeric-0 =
  built-in-default convention (0 → 8), so the working-set floor is always ≥ 1. It can
  never be configured to zero; an explicit small value (e.g. 1) just narrows it.
- **Identifiers are conserved, not bodies.** When an old tool-result body is elided,
  the Coordinate Closet preserves the exact paths/ids/hashes it contained (plus the
  body's head+tail excerpt), so references survive. The *omitted middle* of an old
  body is not retained.
- **Recovery is the agent re-issuing a tool call** (e.g. re-reading a conserved path). There is **no automatic inline re-fetch**. If a re-fetch fails, the agent handles
  it as an ordinary tool error. This refetch is the accepted cost of default-on.
- Coordinate fidelity (0 lost identifiers) was validated on a captured long session in
  the deterministic-fold close-out.

Operators who want the prior behavior opt out per lever, e.g.:

```yaml
reduce:
  history_fold: false   # keep full history (no rolling skeleton)
  compress: false       # keep full tool-result bodies
  # delegate_seam: false  # disable the economizer on the delegate path entirely
```

**Config persistence convention.** A fresh default config writes **no** `reduce` or
`compact.coord_closet` block at all. The defaults live in `config_set_defaults`
(`src/config.c`). On save, *default-on* keys (`measure`, `delegate_seam`,
`history_fold`, `compress`, `freeze_guard`, `coord_closet.enabled`) persist only their
non-default **OFF** state; *default-off* keys (`gateway_seam`) persist only their
non-default **ON** state. So an operator opt-out round-trips, and an unmodified config
stays empty. Note `coord_closet.budget_bytes: 0` means **"use the built-in default"**,
not "force a zero-byte closet". To disable the closet, set `enabled: false`.

**Not yet default-on:** the **gateway seam** (`reduce.gateway_seam`, the inbound /v1
path that serves the primary agent) remains off. It is shadow-measure-only until its
request-mutation path and 400-retry-from-pristine circuit breaker are built. The
legacy standalone `fold.*` path also stays opt-in; the economizer supersedes it (when
the economizer produces a reduced view, the legacy `build_fold_view` is skipped).
