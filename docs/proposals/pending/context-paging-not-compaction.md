# Context paging, not compaction

- **State:** proposed.

## Goal

Three properties, in dependency order:

1. **An agent can recall on demand anything that was evicted.** Eviction is
   reversible.
2. **We compact better than the clients do** — from state we recorded as it
   happened, not from a retrospective reading of the transcript.
3. **The user never feels a compaction.** No multi-minute stall, no visible
   boundary event.

These are not three parallel goals. (1) licenses (3): if eviction is reversible,
we can evict early and continuously instead of waiting for a cliff, so the
transcript never approaches the wall and there is no compaction *event* to feel.
(2) is what makes (1) reliable — you can only page something back in if you
conserved an exact coordinate for it, not a model's paraphrase of it.

The deliverable is therefore not a better compactor. It is turning compaction
from a **periodic cliff** into a **continuous paging system**.

## Why the current design cannot get there

Compaction today is an event: `maybe_compact_before_request()`
(`src/posix/agent_runtime.c:240`) samples pressure, and at 80%
(`SESSION_PRESSURE_COMPACT`) rewrites history in one destructive pass. The
summarised messages are deleted. Anything the summary failed to capture is gone.

Because it is destructive it must be *late* — you delay an irreversible lossy
step as long as possible. Because it is late it is *large*. Because it is large
it is a cliff. Every property we want is blocked by the irreversibility.

The client-side compactors (Codex, Claude Code) have the same shape plus a model
call, which is why they cost minutes.

### What we re-derive that we already knew

`session_compact.c` builds its summary by scraping prose: `token_looks_like_path()`
guesses which tokens are paths, `flashback_extract_from_text()` pattern-matches
for errors and decisions (`:196-305`). It is a heuristic reconstruction of facts
the system recorded first-hand and then discarded.

Meanwhile the economizer records those facts exactly:

- `coord_closet` — uuids, shas, paths, refs, handles extracted **verbatim before
  truncation**, stamped `{lane, turn, tool_call, result}`, secrets redacted at
  render, and — critically — `COORD_EVICT_FAIL` rather than silent loss.
- `fold_register` — each assistant turn classified settled (verdict/hazard) vs
  transient (in-progress/executing/blocked), from the agent's own tagging.
- `task_rail` — the plan as a locked state machine with per-step state and
  evidence, explicitly designed to live *outside* the prompt.
- `episode_seal` — file inventory plus conclusion, with a file-touch auto-recall
  predicate.
- `fold_recall` — a page table of evicted coordinates with a residency TTL and a
  refetch path.

`session_compact` consumes **none** of it. Verified: no reference to closet,
rail, seal, register or recall anywhere in the file. The recorders and the
compactor shipped as separate slices and were never joined.

## Verified shortfall inventory

| # | Shortfall | Evidence |
|---|---|---|
| A | `session_compact` consumes no recorded state; re-derives by scraping prose | `src/server/session_compact.c:196-305` |
| B1 | `task_rail` has zero live callers; DB1 target exists, unbound | `src/db1/checkpoints.c:30` has `session_id` + `snapshot` |
| B2 | `episode_seal` has zero live callers; DB2 target exists, unbound | `memory_units`, `unit_type="episode_seal"` |
| B3 | `fold_recall` has zero live callers, default-off, resolver never wired | `fold_recall.h` "Default-off" |
| C | `reduce_state_t` is a stack local — no record survives the run | `src/posix/agent_runtime.c:600` |
| D1 | `gw_mutate_upstream_ok` refuses Anthropic egress unconditionally | `gateway_mutate_wire.c:52` |
| D2 | Gateway session key is per-identity, not per-conversation | `msg_session_disable.h:33` |
| D3 | Gateway is compress-only, fold deferred | `gateway_mutate_wire.c:113` |
| E | Whether clients defer their own compaction on lower relayed usage | **unverified** |
| F | Baseline never run; rounds-to-resume consumed by nothing | `pending/compaction-quality-baseline.md` |

Live and already recording: `coord_closet` and `fold_register`, config-gated at
`src/posix/agent_runtime.c:773-780`. That is the foundation slice 1 builds on,
and it needs no new recording to start.

## Slices

### S0 — Settle the unverified premise (E)

Run a long Codex session through the gateway with mutation enabled and observe
whether its compaction fires at the usual point. The relay is confirmed
byte-verbatim (`relay_capture_usage`, "without altering it",
`anthropic_http.c:774`), so upstream computes usage on the reduced payload and
the smaller number reaches the client. What is *not* known is whether the client
trusts that number or maintains its own local estimate.

Decisive and cheap. If clients estimate locally, all of D is dead and slices 1-3
stand on their own merits. **Nothing in D may be built before this returns.**

### S1 — Compact from the record, not the transcript (A)

Rewrite the summary builder to consume closet coordinates, register
classifications and rail steps. Delete the prose-scraping heuristics as they are
superseded, not alongside them.

Highest value, no new recording required, and it is the change that makes the
summary deterministic-from-record. Behaviour change only — no refactor in the
same commit.

### S2 — Turn the recorders on (B1, B2, B3, C)

- Bind `task_rail` to DB1 `checkpoints.snapshot` keyed by `session_id`.
- Bind `episode_seal` to DB2 `memory_units`.
- Promote `reduce_state_t` from stack local to session-scoped persisted state.
- Wire `fold_recall`'s resolver to `code_span_get` / `memory_get` and default it on.

C is the one that makes the record survive a session boundary, which is what
turns a per-run structure into an actual memory hierarchy.

### S3 — Continuous paging (the goal)

With recall reliable, move eviction off the 80% trigger and onto a continuous
low-water/high-water discipline: evict steadily from the moment the transcript
starts growing, keeping occupancy well below any threshold. `fold_freeze_t`'s
prefix digest already keeps an unchanged prefix byte-identical, so continuous
eviction stays prompt-cache-warm rather than thrashing it.

This is where "the user never feels a compaction" is actually delivered: there
is no cliff because occupancy never approaches one.

### S4 — Gateway (D1, D2, D3) — **conditional on S0**

Make the Anthropic refusal conditional on pressure rather than absolute: the
cache argument that justifies it inverts once the client is about to compact,
because the client's own compaction rewrites the prefix anyway. Key sessions by
message-prefix digest rather than credential. Enable fold at the wire.

### S5 — Measurement (F)

Run the outstanding baseline from `compaction-quality-baseline.md` and wire
`rounds-to-resume` (`session_compact_result_t.readonly_sigs`) to consume it as a
regression gate on S1 and S3.

## Risks

- **S1 quality regression.** The prose heuristics may be catching something the
  structured record misses. S5's baseline is the guard; if S5 cannot run first,
  S1 ships behind a config flag with the old path retained until measured.
- **S3 recall thrash.** Aggressive eviction with an unreliable resolver degrades
  worse than a late cliff. S3 is gated on S2's resolver being wired and on the
  residency TTL demonstrably preventing re-surfacing loops.
- **S0 kills S4.** Accepted and by design — that is why S0 is first and why S4 is
  last.

## Acceptance

- `session_compact` derives its summary from recorded state; the prose-scraping
  path is deleted, not merely bypassed.
- `task_rail` and `episode_seal` persist across session boundaries and are
  recoverable by `session_id`.
- An agent can page back a coordinate evicted in an earlier session.
- Transcript occupancy under continuous paging stays below the compact threshold
  across a long session, with no single-event boundary.
- The S5 baseline is committed and gates S1/S3 against regression.
- Every criterion above is exercised by a test or a reproducible harness run.
  Anything that cannot be run in-tree is reported validation-pending, not done.
