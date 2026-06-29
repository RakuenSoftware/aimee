# Proposal: Deterministic context folding — turn skeletons, identifier conservation, and cross-session episodes

- **State:** DRAFT — proposed, not started.
- **Author:** JBailes
- **Date:** 2026-06-29
- **Charter roles:** Rewrite, Recall, Extract, Calibrate / Evaluate-Optimize
- **Thesis:** A long agent session should not pay a model round-trip to forget.
  Aimee can fold old conversation turns into compact, *deterministic* skeletons —
  no LLM call, byte-identical between turns so the provider prompt cache stays
  hot — while conserving the exact identifiers (uuids, paths, shas, ports) that
  truncation and summarization silently drop. Sealed work survives across sessions
  as replayable episodes.

## Goal

Sustain hundreds of turns inside a fixed context window with three guarantees the
current envelope work does not yet give us:

1. **Zero-LLM compaction of the transcript itself.** Today Aimee compacts the
   *injected* `<aimee-context>` envelope and individual oversized tool results. It
   does not fold the *conversation history* — the accumulating turn-by-turn record
   that actually exhausts the window on a long session.
2. **No lost coordinates.** Whatever we fold, the literal identifiers an agent
   needs to keep working (record ids, absolute paths, commit shas, ports, issue
   refs) must survive verbatim, not be paraphrased away.
3. **Cache stays hot.** Re-deriving the folded prefix every turn defeats the prompt
   cache. The folded prefix must be reproducible byte-for-byte so the provider
   reads it from cache instead of re-billing it as fresh input.

## §0 What already exists (so we don't rebuild it)

- **Ingress envelope compaction — LIVE/in-progress.** `ingress-compression-and-cache-
  alignment` (pending) and `recall-economy-progressive-disclosure` (done) bound and
  fold the `<aimee-context>` envelope, expose id-addressable follow-up reads
  (`memory_get`, `get_context_block`), and reserve a typed envelope IR
  (`ingress_entry_t`, `ingress_transform_t` in `src/headers/ingress_preinject.h`).
  **This proposal targets the transcript, not the envelope** — they compose.
- **Tool-result compaction — LIVE.** `src/compact.c` does mechanical JSON-structure
  summary + head/tail truncation per oversized tool result. It is the right
  primitive but operates per-result, has no identifier-conservation lane, and does
  not skeletonize whole turns.
- **Cache-aware prefix rewrite — LIVE.** `src/payload_rewrite.c` tracks a stable
  prefix (FNV-1a over system + static context) and defers envelope moves to protect
  the cache (`cache_aware_rewrite_*`, `ingress_cache_placement_enabled`). This is
  the substrate the fold-freeze in §3 builds on.
- **Episode cards — LIVE.** `src/memory_episodes.c` LLM-synthesizes episode
  summaries (`memory_episode_card_generate`, stored as `KIND_EPISODE` memory_units)
  gated by `memory_episode_summaries_enabled`. They are *narrative summaries*, not
  *sealed checkpoints with a file inventory*, and they do not auto-recall on file
  re-touch. §5 adds a **distinct** sealed-checkpoint kind (`KIND_EPISODE_SEAL`) and
  does not overload `KIND_EPISODE`.
- **Checkpoints — LIVE.** DB1 `checkpoints` + `file_snapshots` capture per-session
  snapshots, but there is no portable plan state machine paged back into the turn.
- **Code-span recovery — LIVE.** `code_span_read()` (`src/headers/code_span.h`)
  re-fetches a folded code region with drift detection. This is the recovery half
  of §4 fold-recall; the trigger/index half is missing.

## §1 Rolling fold (deterministic turn skeletonization)

**Principle.** Once a turn is `N` turns behind the head, replace its full body with
a **skeleton**: one line per tool call, plus a budgeted slice of retained reasoning.
The newest band of turns stays at full fidelity. No model is invoked — the fold is a
pure CPU transform over the existing transcript.

- **Skeleton form.** Each historical tool call collapses to a single deterministic
  line: `read src/foo.c → 412 lines`, `$ make → ok (rc=0)`, `search "ingress_entry"
  → 7 hits`. Assistant prose collapses to a bounded retained-reasoning excerpt (the
  conclusion/verdict band, see §6), not the full body.
- **Non-destructive.** Raw history is append-only and never mutated. The fold
  produces a *view* — a synthetic assistant/user pair carrying the skeleton with a
  self-documenting preamble ("the turns below are folded; exact identifiers are
  conserved in the closet, full bodies are re-openable") — that is what we send to
  the provider. The raw tail is preserved for re-fold and for replay.
- **Atomic tool pairs.** A `tool_use` and its matching `tool_result` are a single
  fold unit — they fold together or not at all. A folded region renders as plain
  **non-tool** assistant/user text (the skeleton lines), never as an orphaned
  `tool_use` with no result or vice-versa, which providers reject. Multi-call,
  parallel-call, and failed-call turns must all collapse to whole skeleton lines.
  This is a hard provider-validity rule, not a nicety (see §3 golden gate).
- **Band, not threshold.** Folding is governed by a *retained band* (most-recent
  turns kept whole) and a *tail cap* (rolling size limit before the fold advances),
  resolved per model in §7 — not a single global char threshold.

Concretely this is a new transcript-level pass (proposed `src/context_fold.c`)
sitting between turn assembly and `payload_rewrite`, reusing `compact.c`'s
truncation primitives for the per-line collapse.

## §2 Coordinate Closet (verbatim identifier conservation)

**Principle.** Skeletonizing a turn must never destroy a value the agent cannot
reconstruct. Before a turn is folded, extract its **opaque coordinates** and pin
them verbatim in a conserved block that rides along with the skeleton.

- **Nominate.** A deterministic extractor flags: uuids, long hex hashes / commit
  shas, absolute and repo-relative paths, digit-bearing key=value pairs
  (`port=3002`, `pid=8841`), issue/PR refs (`#778`), and `handle:<id>` /
  `memory:<id>` tokens Aimee already mints. (This is the inverse of the heuristics
  `compact.c` uses to decide *what to drop* — here we decide *what to keep*.)
- **Conserve with a label.** Each coordinate gets a stable, deterministic context
  label derived from its surrounding key, e.g. `7fd5835b ⟦changelog_id⟧`,
  `3002 ⟦llm_port⟧`. Identical inputs ⇒ identical labels — this is a **hard**
  requirement for §3 byte-identity, so the derivation is fully specified, not
  heuristic:
  - **Normalize.** Coordinate values are Unicode-NFC-normalized; JSON-sourced
    values are canonicalized (compact form, sorted keys, no insignificant
    whitespace) before extraction.
  - **Label.** The label is derived from the nearest key token (snake/camel
    normalized to snake), falling back to a type tag (`uuid`/`sha`/`path`/`port`/
    `ref`) when no key is in scope. No model, no clock, no randomness.
  - **Emission order.** Entries emit in a total order: `(lane, label,
    first-occurrence byte offset)`, tie-broken by the stable
    `(turn_id, tool_call_id, result_index)`. Same transcript ⇒ same bytes
    regardless of internal hash-map iteration order.
  - Unit tests assert byte-identical closet output for shuffled-input and
    repeated-run permutations.
- **Capped lanes with provenance.** Coordinates from agent/tool turns and from
  **user-pasted text** live in separate capped lanes so a giant paste cannot evict
  the agent's own working identifiers. User-lane entries are **provenance-tagged**
  (distinct lane marker) and are *quarantined*: they render as untrusted and never
  mint an agent-trusted label nor auto-promote into durable memory (§5) without
  explicit agent action. This blocks a paste like `3002 ⟦llm_port⟧` from
  impersonating a conserved agent coordinate — a prompt-injection vector. A
  secret/PII filter runs before any user-lane coordinate is persisted or recalled.
- **No silent loss (P1 invariant).** The closet is bounded by the §7 budget, but
  dropping a *nominated* coordinate is treated as a **fold failure**, not silent
  FIFO eviction: the fold either (a) sizes the cap to fit the conserved set, (b)
  pages the overflow to a DB-backed spill resolvable via §4, or (c) refuses to
  advance the fold for that turn. Whichever tier applies is logged. This is what
  makes the "0 lost coordinates" validation gate achievable rather than
  self-contradictory.

This is the single highest-leverage piece: it closes a real correctness hole where
head/tail truncation can amputate the exact id the next tool call needs.

## §3 Fold freeze (byte-identical prefix reuse)

**Principle.** The folded prefix is computed once and **reused byte-for-byte**
across subsequent turns; only the raw tail grows. The provider then reads the prefix
from cache instead of re-billing it.

- **Freeze the fold.** After a fold advances, its rendered view (skeleton + closet)
  is frozen. New turns append only to the raw tail. The frozen bytes are emitted
  identically next turn — this is what keeps cache-read rates high.
- **Recompute only at epoch boundaries.** Re-derive the frozen prefix only when:
  first call; freeze TTL expired; tail exceeded its cap; the retained band's
  conclusions/claims changed; or a boundary rewrite is forced. Otherwise reuse.
- **Single prefix-state owner (decision).** `payload_rewrite.c` remains the *sole*
  owner of the cache-prefix state and the FNV-1a stable-prefix hash. This proposal
  does **not** introduce a second writer. Instead it adopts a **layered contract**:
  the transcript fold registers its frozen span with `payload_rewrite` via a new
  helper (proposed `fold_emit_stable_span`), and the pending
  `ingress-compression-and-cache-alignment` work registers the envelope span the
  same way. `payload_rewrite` composes the registered spans into one prefix and
  hashes once. Two independent writers mutating the prefix — the cache-thrash
  hazard the review flagged — is thereby ruled out by construction. §0 reflects
  that the two proposals *converge on one owner* rather than forking it.
- **Pipeline order (normative).** The stages run in exactly this order so the frozen
  bytes are never mutated after hashing:
  1. raw DB1 transcript (append-only) →
  2. transcript fold view + frozen-prefix assembly (§1/§2) →
  3. provider-shape normalization (§1 atomic-pair rendering for Anthropic blocks /
     OpenAI `tool_calls` / Gemini `parts`) →
  4. `payload_rewrite` stable-span registration + single cache-prefix hash →
  5. final payload to the provider.
  The fold pass must sit at stage 2 — it sees the assembled history but runs
  **before** the stage-4 hash; nothing downstream may rewrite the synthetic
  assistant/user pair.
- **Builds on what exists.** This extends `payload_rewrite.c`'s prefix tracking and
  cache-aware deferral from the *envelope* to the *folded transcript*. Config:
  reuse the `cache_aware_rewrite_*` family; add `fold_freeze_enabled`,
  `fold_freeze_ttl_ms`, `fold_tail_cap_tokens`.
- **Honesty.** Telemetry is measured provider cache-read tokens, not a character-
  count estimate — consistent with the honest-benchmark framing already adopted for
  ingress compression.

## §4 Fold recall (ambient page-in)

**Principle.** When the agent re-engages something it had folded away, page the
relevant folded content back in — as a bounded recall card on the hot tail, not by
thawing the whole prefix.

- **Page table.** A fold index maps folded content → the path/handle/id that would
  re-summon it (proposed `context_fold_index`, in-memory per session, optionally
  persisted).
- **Trigger.** When a new turn reads/claims a path or cites a `handle:`/`memory:`
  token present in the index, append a budgeted recall card to the raw tail (cache
  stays hot) and let it re-fold at the next epoch.
- **Recovery resolvers already exist.** Code regions resolve via `code_span_read()`
  (with drift detection); memory via `memory_get`. This proposal adds the *trigger
  and index*, not new resolvers.
- **Anti-thrash.** Residency TTL keeps a recalled card resident for a few turns so a
  fold→recall→fold oscillation cannot churn the cache.

## §5 Episodic recall (sealed work across sessions)

**Principle.** When a unit of work seals, persist it as a **replayable episode** —
the *set of files touched* plus the agent's conclusions — and auto-recall it when
any later session touches a member file again.

- **Seal contract.** An episode is a checkpoint, not a paraphrase: file inventory +
  conclusion band (harvested per §6 trust tags) + the relevant closet coordinates.
- **Auto-recall by file-touch.** On session entry / first touch of a file, look up
  episodes whose inventory contains it and surface them as bounded recall cards.
  This is the gap over today's `memory_episodes.c`, which produces narrative cards
  with no file-touch trigger.
- **Distinct kind (avoid collision).** Today's `KIND_EPISODE` memory_units are
  *narrative cards* from `memory_episodes.c`. A sealed checkpoint is a different
  thing, so it gets its own memory_unit kind (proposed `KIND_EPISODE_SEAL`) rather
  than overloading `KIND_EPISODE` — querying and trust semantics stay unambiguous.
  §0 is updated to state both kinds and their distinction. Reuse the storage
  plumbing and `memory_episode_cards_query` shape; add the file-inventory index and
  the touch trigger on top.

## §6 Register grammar (trust tags on assistant turns)

**Principle.** Tag each assistant turn with a one-glyph **register** so the fold and
the episode harvester know what is settled versus in-flight.

- **Registers.** in-progress / executing / verdict / hazard / blocked (a small fixed
  set; exact glyphs TBD). The agent opens a turn with its register; a deterministic
  parser classifies it.
- **Why it matters here.** (a) The §1 retained-reasoning band keeps *verdict* and
  *hazard* turns and drops *in-progress* chatter — better fold fidelity per byte.
  (b) The §5 harvester only seals settled conclusions into durable memory, so
  transient work self-excludes. This complements `fact_gate_verdict_t`
  (`src/memory_fact_gate.c`), which gates *facts*, not *turns*.
- **Soft dependency.** The fold degrades gracefully if turns are untagged (treat as
  in-progress); the grammar improves fidelity, it is not load-bearing for §1–§3.

## §7 Per-model context budget

**Principle.** Replace scattered global knobs with a single **model → budget**
resolver that turns a model choice into the fold parameters deterministically.

- **Resolved knobs.** context-window tokens, retained-band tokens, fold-tail-cap
  tokens, pressure-ceiling (hard-fold trigger), prefix-saturation (freeze recompute
  trigger), closet cap, eviction policy.
- **Table-driven.** A pre-seeded table for the models Aimee actually routes to,
  with an explicit `context_window_tokens` fallback for unknown models. `model_
  context_window()` (`src/model_registry.c`) already knows window sizes — this
  layers the fold knobs on top.
- **Deterministic resolution.** The resolver is a pure function of
  `(model, transcript, config)` — it is a **prerequisite** for §1/§3 byte-identity,
  not a later tuning pass. Any per-tool override that still affects a fold
  (`memory_context_budget_*`, `compact_*`) must be either frozen for the freeze TTL
  or serialized into the fold-input hash, so the same inputs cannot resolve to two
  different budgets across turns. The Determinism gate covers this.
- **Subsumes** the ad-hoc `memory_context_budget_*`, `ingress_compress_min_chars`,
  and `compact_*` globals over time (those stay as per-tool overrides, under the
  freeze rule above).

## §8 Task rail (portable plan state, outside the prompt)

**Principle.** Track the multi-step plan as a small serialized **state machine** that
lives outside the prompt and pages back a compact "where we are" card — so the plan
survives folds, epoch rebirths, and session boundaries without re-narration.

- **Shape.** A locked rail of steps with status (`pending`/`reserved`/`done` +
  evidence handle), plus `serialize`/`restore` to JSON. Reuses DB1 `checkpoints`
  for persistence.
- **Hard-epoch rebirth.** When the window is force-reset, the wake seed is computed
  deterministically from the rail + closet + sealed episodes — derived from the
  trace, not model-summarized.

## Phasing

Each phase is independently shippable behind a default-off flag, validated, then
considered for default-on per the established off-reasons discipline.

- **P1 — Coordinate Closet (§2) as a `compact.c` lane.** Highest value, smallest
  blast radius, no transcript surgery. Nominate + conserve (canonical label algo) +
  render, wired into the existing tool-result and envelope compaction paths.
  Includes the no-silent-loss invariant and the user-lane provenance guard.
- **P1.5 — Pipeline-order spike + per-model budget resolver (§7).** Small spike that
  fixes the fold-pass insertion point (§3 pipeline order) and lands the deterministic
  `(model, transcript, config)` budget resolver. **Prerequisite for P2** — byte-
  identity cannot be validated without a deterministic budget, so §7 lands here, not
  after the fold.
- **P2 — Rolling fold + freeze (§1, §3).** The transcript-level pass (atomic tool
  pairs, provider-shape rendering) + byte-identical prefix reuse via the single
  `payload_rewrite` owner. The core economic win.
- **P3 — Register grammar (§6).** Fidelity: retained-band selection + episode-seal
  harvest gating.
- **P4 — Fold recall (§4).** Page-table + trigger over existing resolvers (also backs
  the §2 coordinate spill tier).
- **P5 — Sealed episodes + file-touch recall (§5) + task rail (§8).** Cross-session
  memory, on the distinct `KIND_EPISODE_SEAL` kind.

## Validation gates

- **Fidelity.** On a captured long session (replayable-verification corpus): fact-
  retention ≥ target after folding, measured by re-asking the agent for the
  conserved coordinates and for prior conclusions. **0 lost coordinates** is a hard
  gate for P1 — made achievable by the §2 no-silent-loss tiering (size / spill /
  refuse-to-fold), not by hoping the cap never fills.
- **Cache.** Measured provider cache-read token share before/after P2 on a real
  multi-hundred-turn run — not a character estimate.
- **Cost.** End-to-end token cost vs. the current truncation path and vs. an LLM-
  summarization baseline, on the same corpus.
- **Determinism.** Same `(transcript, model, config)` ⇒ byte-identical fold view and
  closet labels, including under shuffled internal iteration order and frozen/
  serialized per-tool overrides (unit-tested), since §3 depends on it.
- **Provider validity.** Golden-snapshot tests assert byte-identical *final payloads*
  across turns and valid tool-call/result pairing for Anthropic blocks, OpenAI
  `tool_calls`, and Gemini `parts` — including multi-call, parallel-call, and
  failed-call turns. (Trips `test_mcp_tools_golden`; regenerate per the golden-test
  procedure.)
- **No silent loss.** Every closet/tail/recall eviction is logged; a fold that would
  drop a nominated coordinate fails rather than truncating it.

## Risks / open questions

- **Insertion point.** Folding the transcript interacts with `server-owned-turn-
  lifecycle` and `payload_rewrite` ordering — the fold pass must run where it sees
  the assembled history but before cache-prefix hashing. Needs a wiring spike.
- **Provider shape.** Skeleton + closet must render validly across Anthropic content
  blocks, OpenAI `tool_calls`, and Gemini `parts` without breaking tool-call/result
  pairing. Golden-snapshot coverage required (trips `test_mcp_tools_golden`).
- **Overlap with ingress-compression.** *Resolved (see §3):* a single prefix-state
  owner (`payload_rewrite`) with both features registering spans via
  `fold_emit_stable_span`. This proposal layers above that work rather than forking
  the prefix. The remaining open item is purely sequencing — whether the two land in
  one PR series or two.
- **Register grammar is a prompt contract.** It only holds if the agent reliably
  emits registers; the fold must stay correct when it doesn't (treated as soft).

## Design review (roundtable, 2026-06-29)

A 6-panelist architecture roundtable (`--mode review --rounds 2`, 0 participants
failed) raised 6 blocking and 3 suggestion items. All were under-specifications
rather than rejections of the design; each is resolved above:

- **B0 — prefix-state ownership (§3 vs ingress-compression).** Resolved: single
  owner `payload_rewrite`; both features register spans (`fold_emit_stable_span`).
- **B1 — fold-pass insertion point.** Resolved: normative 5-stage pipeline in §3;
  fold runs at stage 2, before the stage-4 cache hash.
- **B2 — closet label determinism.** Resolved: canonical NFC + JSON-canonical +
  total emission order in §2, with shuffled-input unit tests.
- **B3 — FIFO eviction vs "0 lost coordinates".** Resolved: nominated-coordinate
  drop is a fold failure (size / spill / refuse), not silent FIFO (§2, gates).
- **B4 — atomic tool_use/tool_result pairing.** Resolved: atomic fold-unit rule in
  §1; folded regions render as plain non-tool text; golden gate in validation.
- **B5 — §7 budget scheduled after the fold it gates.** Resolved: §7 moved into
  P1.5 as a prerequisite for P2 byte-identity.
- **S7 — `KIND_EPISODE` collision.** Resolved: §5 uses a distinct
  `KIND_EPISODE_SEAL` kind; §0 disambiguates.
- **S8 — per-tool overrides break byte-identity.** Resolved: frozen-for-TTL or
  serialized into the fold-input hash (§7, Determinism gate).
- **S9 — user-lane label injection (prompt-injection vector).** Resolved: user-lane
  coordinates are provenance-tagged, quarantined (no agent-trusted label, no auto-
  promotion), and secret/PII-filtered (§2).
