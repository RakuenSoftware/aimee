# Context paging S2–S5: turn the recorders on, then stop compacting

- **State:** proposed.
- **Parent:** [`context-paging-not-compaction.md`](context-paging-not-compaction.md) (S0–S5 overview)

## Where this picks up

S1 landed (#2507). `session_compact` can now derive its summary from recorded state —
Coordinate Closet coordinates conserved verbatim, `fold_register`'s settled/hazard
classification — instead of scraping prose for path-shaped tokens and error-ish
keywords. It ships behind `compact.from_record`, **default-off**.

It is default-off because it was measured, and the measurement said not yet.

| | legacy | record |
|---|---|---|
| all fixtures | 11/20 (55%) | **15/20 (75%)** |
| fixtures matching *today's* defaults | **10/14** | **10/14** |

The second row is the one that matters. Every remaining record-path loss has a single
cause: `fold_register_enabled` is off, so real transcripts carry no `[verdict]` /
`[hazard]` tags, so Decisions and Blocked come back empty. Under today's configuration
the two derivations are **dead even** — the record path trades decision recall for
identifier recall. The 75% is earned only on fixtures that assume register tagging.

That single fact drives most of what follows: **the highest-value work is not more
compactor cleverness, it is turning on the recorders whose output the compactor is now
able to read.**

Harness: `benchmarks/compaction-quality`, `make -C src compaction-retention-probe`.

## The goal, restated

Compaction today is destructive, so it must be late; late means large; large means a
cliff the user waits on. Every property we want is blocked by irreversibility. The
programme turns compaction from a **periodic cliff** into **continuous paging**:

1. an agent can recall on demand anything evicted (reversible), which licenses
2. evicting early and often, so occupancy never approaches a threshold, so
3. there is no compaction *event* for the user to feel.

S1 improved *what the summary says*. S2–S3 are what make eviction reversible and
continuous. S4 extends it to the wire. S5 keeps all of it honest.

---

## S2 — Turn the recorders on

Four components are built, unit-tested, and **have zero live callers**. Their storage
targets already exist; only the bindings are missing.

### S2a — `task_rail` → DB1 `checkpoints`

The plan as a locked state machine with per-step state and evidence, explicitly designed
to live *outside* the prompt so it survives folds and epochs.

- Round-trip is complete: `task_rail_serialize()` and `task_rail_restore(r, json)`
  (`task_rail.h:63,69`).
- Storage exists: `checkpoints (id, task_id, session_id, label, snapshot, created_at)`
  (`src/db1/schema.sql:4`), with `db1_checkpoint_insert(label, session_id, task_id,
  snapshot_json, out)` (`checkpoints.h:25`).

**Gap to close:** the read side is `db1_checkpoint_get(id)` and
`db1_checkpoint_list(limit, …)`. There is **no lookup by `session_id`**, which is exactly
the query a resuming session needs. Add one rather than making callers list-and-filter —
a list-and-filter resume silently degrades as the table grows.

**Acceptance:** a rail written in one session is restored byte-identical in the next,
keyed by `session_id`; a step ack'd before a boundary is still `DONE` after it.

### S2b — `episode_seal` → DB2 `memory_units`

File inventory plus conclusion, with a file-touch auto-recall predicate.

- Round-trip complete: `episode_seal_serialize()` / `episode_seal_parse()`
  (`episode_seal.h:49,54`).
- Storage target is the existing `memory_units` row with a distinct
  `EPISODE_SEAL_UNIT_TYPE` — **no schema column, no migration** (`episode_seal.h:24`).

**Acceptance:** sealing an episode then touching a member file in a later session
surfaces that episode's conclusion. This is the first cross-session recall in the
programme and should be demonstrated end to end, not just unit-tested.

### S2c — `reduce_state_t` off the stack

`reduce_state_t agent_reduce_state;` is a **stack local** (`src/posix/agent_runtime.c:629`).
It holds the fold freeze boundary and its prefix digest, so today the record dies with
the run — there is no continuity to build S3 on.

Promote it to session-scoped persisted state. `checkpoints.snapshot` is the natural home
alongside the rail, keyed by the same `session_id`.

**Care required:** the freeze boundary is a *cache-warmth* optimisation. A restored
boundary that no longer matches the transcript must not be trusted — `fold_freeze_t`
already carries `prefix_digest` for exactly this, and a mismatch must force an epoch
rather than serve a stale prefix. Restoring the digest without honouring it would be
worse than not persisting at all.

### S2d — `fold_recall` resolver, and default it on

The page table: evicted coordinates with a residency TTL. `fold_recall_detect(ix,
turn_text, turn, ttl_turns, out)` already emits bounded recall hints when a folded
coordinate is re-touched (`fold_recall.h:49`).

Both resolver targets exist as real tools: **`code_span_get`**
(`mcp_tools_extended.c:91`) and **`memory_get`** (`mcp_tools.c:253`). The missing piece
is the wiring from hint → fetch, plus flipping `fold_recall_enabled` on.

**This is the keystone.** Without it, eviction is destructive and S3 is unsafe at any
aggression. With it, eviction becomes paging.

**Acceptance:** a coordinate evicted N turns ago, referenced again, is paged back in
without the agent re-deriving it; the TTL demonstrably prevents re-surfacing the same
coordinate every turn.

---

## S3 — Continuous paging

With recall reliable, move eviction off the `SESSION_PRESSURE_COMPACT` trigger (80%) and
onto a **low/high-water discipline**: evict steadily from early in the session, holding
occupancy in a band far below any threshold.

Design notes:

- **Plug in at the existing seam.** `context_engine.c` is a registry with
  `should_compress(self, state, prompt_tokens, context_length)` and `compress(self,
  messages, focus_topic)`. Continuous paging is a *different engine*, not a rewrite of
  the compactor — which also means it can be selected per-config and compared against
  the compactor on the same harness.
- **Cache warmth is the main risk.** Evicting on every turn rewrites the prefix on every
  turn, which would destroy prompt-cache hits — the opposite of the economizer's
  purpose. `fold_freeze_t`'s frozen boundary plus `prefix_digest` is what makes this
  viable: the folded prefix stays byte-identical until the boundary advances. **Any S3
  implementation that does not reuse the freeze is wrong.**
- **Water marks, not a threshold.** Evict when occupancy crosses the high mark, down to
  the low mark, so the boundary advances in steps rather than continuously.

**Acceptance:** across a long session, occupancy stays inside the band and never reaches
the compact threshold; no single-event boundary appears; measured prompt-cache hit rate
does not regress against the current compactor.

---

## S4 — Gateway — **still gated on S0, which remains unanswered**

S0 asks: *does a client defer its own compaction when we reduce the request and upstream
reports lower `input_tokens`?* The relay is confirmed byte-verbatim
(`relay_capture_usage`, "without altering it", `anthropic_http.c`), so the reduced number
does reach the client. What is unknown is whether the client trusts it or keeps a local
estimate.

**This has still not been tested.** Two attempts failed because delegates could not run
shells (see below), and it needs a live Codex session driven through the gateway with
control and treatment runs, plus `gw_mutate_stats` counters to confirm mutation actually
engaged — a treatment run where mutation silently hard-bypassed looks identical to a
usage-blind client, and that is the main way this experiment produces a false answer.

If S0 says clients estimate locally, **S4 is dead** and should not be built. Nothing in
S4 may start before that answer exists.

If S0 is positive, S4 is: make the Anthropic refusal in `gw_mutate_upstream_ok()`
conditional on pressure rather than absolute (the prompt-cache argument inverts once the
client is about to rewrite the whole prefix anyway); key gateway sessions by
message-prefix digest rather than credential; enable fold at the wire.

---

## S5 — Finish the measurement

Two axes are missing, and both change how much the existing numbers can be trusted.

### S5a — Precision

The retention probe measures **recall only**: did a planted fact survive. It says nothing
about whether the summary also accumulated junk. A derivation that keeps everything
scores 100% and is useless. Until precision is measured, "75%" must not be read as
"better summary" — and the current corpus cannot distinguish the two.

Proposed: plant **distractors** — plausible-looking but irrelevant strings — and score
what fraction of summary content is load-bearing.

### S5b — Rounds-to-resume

The committed metric, and the only one that measures *agent behaviour* rather than string
retention. Machinery exists: `src/server/rounds_to_resume.c`, fed by
`session_compact_result_t.readonly_sigs` captured before the delete. It has never been
run over a corpus.

This needs live agents, so it is the expensive half — and it is the half that would tell
us whether any of this actually helps an agent get its work done.

---

## Cross-cutting: register tagging is the real unlock

> **Corrected 2026-08-11 — the paragraph this replaces named the wrong lever.** It said
> `fold_register_enabled` defaults off and that this is why the record derivation is a
> lateral move. Both halves were wrong, and the wrong version shipped in #2532 and was
> repeated in #2537 and #2541, so it is corrected here rather than quietly edited.

Two facts from the tree:

1. **The record path does not read that flag.** `session_compact`'s record derivation
   calls `fold_register_parse` unconditionally — `fold_register_enabled` appears **zero**
   times in `src/server/session_compact.c`. The flag gates the *fold's* skeleton
   annotation (`context_fold.c:79`), which is a different consumer.
2. **Nothing asks agents to emit registers.** There is no `prompts/` directory; the system
   prompt is `PROMPT_STANDARD_TEXT` in `src/prompts.c`, and it never mentions
   `[verdict]` / `[hazard]` / `[wip]`. The only occurrences of those tags in the whole
   repository are unit tests and this document.

So flipping `fold_register_enabled` would change nothing for the compactor. Real
transcripts carry no registers because **agents are never asked for them**, which on its
own explains the measured result: record 15/20 vs legacy 11/20 overall, and dead even at
10/14 on fixtures matching real transcripts.

The actual unlock is a **prompt change** in `src/prompts.c`. That is a behaviour change
for every agent, and an agent that tags *unreliably* is worse than one that never tags —
a mis-tagged `[verdict]` promotes a guess into the summary as a settled fact, which is the
failure mode the precision axis was built to catch.

Sequence: add the instruction (gated, so it can be enabled for one server rather than
everywhere) → measure tagging **accuracy** → re-run `benchmarks/compaction-quality` →
only then revisit the `compact.from_record` default.

## Risks

- **S3 thrash.** Aggressive eviction with an unreliable resolver degrades worse than a
  late cliff. S3 must not start until S2d is wired and its TTL is shown to prevent
  re-surfacing loops.
- **Cache regression.** See S3; the freeze is mandatory, not optional.
- **Persisted-state staleness.** S2c must honour `prefix_digest` or it will serve
  obsolete prefixes — a subtle, expensive failure.
- **Mis-tagging.** See above; tagging accuracy is a prerequisite, not an afterthought.
- **Measuring the wrong thing.** Ground truth in the corpus is planted by hand
  specifically so the record path cannot score well by construction. Any new fixture must
  preserve that discipline: never derive expectations with `coord_closet` or
  `fold_register`.

## Delegation is still blocked

S2–S5 is parallelisable work — four largely independent bindings in S2 alone — but it
cannot be delegated today. Delegates cannot run shells: they reach co-located execution
(rather than their own container), and co-located isolation was refused because
`sandbox_available()` treated container detection as a verdict. #2513 fixes that refusal
and is unmerged/undeployed; the deeper issue — delegates not getting a container
workspace provider at all — is untouched.

Until a trivial `echo` delegate demonstrably runs, S2–S5 is inline work and should be
planned as such.

## Sequencing

1. **S2d** (`fold_recall` resolver) — keystone; everything downstream depends on
   eviction being reversible.
2. **S2a / S2b / S2c** — independent of each other; S2c needs the digest discipline.
3. **S5a** (precision) — cheap, and it retro-validates the S1 numbers already recorded.
4. **S3** — only after S2d.
5. **Register tagging + re-measure** — decides the `compact.from_record` default.
6. **S0**, then **S4** if and only if S0 is positive.
7. **S5b** (rounds-to-resume) — most expensive, most informative; can run in parallel
   once agents are available.

## Acceptance for the programme

- A coordinate evicted in an earlier **session** can be paged back in.
- `task_rail` and `episode_seal` survive session boundaries and are recoverable by
  `session_id`.
- Long-session occupancy stays in band with no single-event boundary and no prompt-cache
  regression.
- Retention *and* precision are both measured, and rounds-to-resume has a committed
  baseline.
- Anything that cannot be exercised in-tree is reported validation-pending, never
  silently marked done.

---

# Amendments (2026-08-11), after implementing S2c–S2d and S5a

Three things this proposal got wrong, each found by checking the tree rather than by
re-reading the plan. Recorded here rather than silently fixed, because the reasoning
they replace is the reasoning a reviewer would otherwise inherit.

## 1. S2a / S2b were mis-sequenced: production is the gap, not persistence

Verified: `task_rail_start` and `episode_seal_set_conclusion` / `_add_file` have **zero
live callers**. Nothing creates a rail. Nothing seals an episode.

"Bind them to storage" would therefore persist data that is never produced — precisely
the *installed and ready is not the same as used* failure this document warns about
elsewhere. Both slices need a **producer** first:

- `task_rail` needs the agent to declare a plan, which is a prompt/behaviour change.
- `episode_seal` has a natural producer already available: at a fold boundary, the
  evicted region yields a file inventory (closet PATH coordinates) and a conclusion
  (register-tagged verdict turns). Both ingredients now exist from S1 and S2d.

Sequence for these two is **producer → measure → persist**, not persist-first.

## 2. S3 cannot live at the `context_engine` seam

The proposal says S3 plugs into `context_engine.c` *and* that reusing the fold freeze is
mandatory, or continuous eviction rewrites the prefix every turn and destroys prompt-cache
hits. Those two requirements are incompatible at that seam:

```c
int (*compress)(struct context_engine *self, void *messages, const char *focus_topic);
```

There is **no per-conversation state parameter**, so an engine implemented there cannot
carry `fold_freeze_t` and cannot keep a stable boundary. Building S3 as specified would
produce exactly the cache regression the same document forbids.

S3 belongs in the **reducer path**, which already owns `reduce_state_t` — freeze,
prefix digest, and (since S2c) a page table that survives the run. Either that, or the
`context_engine` ABI grows a state parameter first; the reducer path is the smaller
change and the one with the state already in it.

## 3. Continuous paging mostly EXISTS — it is the fold, and it is switched off

The framing of "build continuous paging" was wrong. The mechanism is already written:

| | trigger | shape | freeze-aware | conserves identifiers |
|---|---|---|---|---|
| `session_compact` | 80% pressure | one destructive boundary | no | only via S1's record path |
| `context_fold_view` | every turn | rolling skeleton | **yes** | **yes** (Coordinate Closet) |

The fold is incremental, freeze-aware, closet-conserving, and now reversible via the S2d
page table. It is `fold_enabled = 0` — **default-off** (`config.c:731`). Meanwhile the
cliff compactor is default-on.

So S3 is not new machinery. It is:

1. Enable the fold so eviction is continuous.
2. Let the 80% compactor become the rare fallback rather than the normal path.
3. Only then consider water marks, which are a refinement of a mechanism that is already
   running rather than a thing to build from scratch.

### What gates that flip

Flipping `fold_enabled` is a large behaviour change and must not be done on argument
alone — the same discipline that reversed the `compact.from_record` verdict twice.

- **Prompt-cache impact is the main risk and is unmeasured.** The freeze exists to keep
  the folded prefix byte-identical; whether it actually holds across real turns needs
  measuring against realized `cache_read` / `cache_write` token counts, not reasoning.
- **Quality** should be compared on the existing harness
  (`benchmarks/compaction-quality`), which currently scores `session_compact` summaries
  only. Comparing a fold view against a summary needs a harness extension, because they
  are different artefacts.

Recommended next slice: **measure the fold against the compactor** on retention,
precision, and cache behaviour — then flip, rather than flip and hope.
