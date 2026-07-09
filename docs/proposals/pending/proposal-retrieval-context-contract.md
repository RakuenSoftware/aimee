# Proposal: Binding retrieval context-contract for agents + a survey of context-engine ideas

- **State:** proposed (pending — not started)
- **Charter roles:** Recall / Rank-Fuse / Calibrate / Plan-Search / Enforce / Gate-Promote

## Thesis

A public open-source *context engine* for coding assistants has converged on a
handful of disciplines for keeping an agent from burning tokens on exploration.
Read against Aimee, **most of its headline mechanisms already exist here** — the
semantic graph, the attention-weighted destructive-op guard, per-intent retrieval
budgets, a confidence scorer, tool-output condensation, episodic memory. The one
idea Aimee does **not** yet have is the discipline that ties them together: a
**machine-readable context contract** the retriever hands to the agent that both
*pre-loads the right code* and *binds the agent's exploration budget* to the
retriever's confidence. This proposal records the full survey (so we don't
rediscover it), maps every idea to the code that already realizes it, and scopes
the one genuinely-new piece — the context contract — to an implementation plan.

The reference implementation is described in concept terms only; nothing below
depends on adopting its (proprietary) graph engine, which Aimee's KB already
exceeds (2,759 files / 29,803 defs indexed, projection-graph, cross-repo sweep).

## §0 What already exists (DRY map)

Every idea in the survey is scored against current code. **Do not rebuild the
left column.**

| External concept | Aimee's existing surface | Verdict |
| --- | --- | --- |
| Attention-weighted destructive-op guard ("undo shield") | `cli_attention_guard.c` — same model, same weights (Read=2, edit=8), recency decay, exit-2 hard block, `$AIMEE_HOME/.cache/attention/<session>.json` | **Have it.** Do nothing. |
| Semantic graph: files/symbols/imports/call-chains | KB index + projection-graph + cross-repo sweep (`#1169`), `docs/CODE_INTELLIGENCE.md` | **Have it (stronger).** |
| Per-intent / per-turn retrieval budgets | `retrieval_plan_for_intent` + `retrieval_plan_t` (`memory_context.c`) — `kind_budget[]`, `recency_weight`, `include_l3` per DEBUG/PLAN/REVIEW/DEPLOY | **Have it (internal).** |
| Retrieval confidence scoring | `memory_retrieval_confidence` + `retrieval_confidence_t` (`memory_context.c`); continuous blended `score` | **Have it (internal, score-only).** |
| Symbol-scoped reads (`file::symbol` → just the def span) | `marshal_preload_append_symbol` (`cli_v1_routes.c`) resolves via `aimee index find`, preloads a ±heuristic excerpt | **Have it (heuristic window).** Refine, don't rebuild. |
| Pre-loading code into the agent prompt | `marshal_build_preload_context` + delegate `--files` / `--context-file` / `--context-dir` / `--context SYMS` (`cmd_agent_delegate.c`, `cli_v1_routes.c`) | **Have the plumbing.** Missing: auto-selection. |
| Tool-output compaction + realized-savings telemetry | `tool_condense.c` | **Have it.** |
| Episodic memory ("graph knows WHERE, episodes know HOW") | delegation episodes with confidence scores (project memory), `docs/KNOWLEDGE.md` | **Have it.** |
| Graph-derived code auditor (dead exports, cycles, dupes…) | `cli_code_audit.c` | **Have it (partial).** Extend, don't rebuild. |
| Learned ranking weights | pending `learning-to-rank-weight-fitting` proposal; `kb_ranker.c` | **Tracked already.** |

**The only clean gap:** the confidence + budgets computed by
`retrieval_plan_for_intent` / `memory_retrieval_confidence` never leave the memory
assembler, and the enforcement cap is a **static, session-wide** config scalar.
Nothing hands the delegate a *per-task binding* "here are the files, here is your
grep/read budget, stop when spent" contract. That is Part II.

## Survey — ideas ranked by payoff, each tied to existing code

Grouped by what work they actually imply.

### Tier A — already covered; adopt nothing, just validation

- **Attention-weighted destructive-op guard.** `cli_attention_guard.c` is a
  near-identical independent implementation. No action.
- **Episodic memory / "compounding" recall.** Aimee already captures delegation
  episodes with confidence. No action beyond what's tracked in `KNOWLEDGE.md`.
- **Tool-output condensation with savings telemetry.** `tool_condense.c`. No action.

### Tier B — refinements to surfaces that already exist (small, high-DRY)

1. **Exact-span symbol reads (refine `marshal_preload_append_symbol`).**
   Today it preloads a fixed heuristic window around the definition line. The index
   already knows each definition's true span; emit `file::symbol` refs from
   retrieval and slice the *exact* `line_start..line_end` instead of a heuristic
   window. Pure token savings on a path that already runs. **Recall.**

2. **Feed the attention log back into ranking ("compounding context").**
   `cli_attention_guard.c` already maintains a per-session attention log; the KB
   ranker doesn't read it. Up-weight recently-read/edited files/symbols in
   `retrieval_plan_for_intent` scoring using that existing signal. One signal, two
   consumers — no new collection. **Rank-Fuse.**

3. **Re-prime durable context after window compaction.** Aimee compacts context
   (`tool_condense.c`, `memory_context.c` window compaction). Add a re-assert of
   the stable project pack immediately post-compaction so the project map survives.
   Mirrors a "PreCompact re-prime" hook. **Recall / Reflect.**

4. **Explicit per-turn token-governance knobs.** Surface the *already-computed*
   budgets (max chars/file, read budget/turn, one-retrieve-per-turn, fallback-grep
   cap) as operator config alongside the 13 curator flags already in the GUI
   (`#1170`). Makes implicit behavior governable. **Enforce.**

### Tier C — genuinely new to Aimee

5. **Binding retrieval context-contract for agents.** *(Part II — the scoped
   deliverable.)* Surface confidence + caps from the memory assembler to the
   delegate as an enforced contract. **This is the item worth building first.**

6. **Extend the code auditor with persist-and-re-inject.** The transferable trick
   isn't the checks (`cli_code_audit.c` has several) — it's writing findings to a
   TTL'd context blob that auto-injects into subsequent session primes for N days,
   so the agent starts each session already knowing the top debt without being
   re-told. Wire audit output into `cli_session_start.c` priming with an expiry.
   **Detect-Cluster / Reflect / Recall.**

7. **Context-preserving failover on rate-limit.** On a 429/529/overload from one
   delegate backend, transparently re-issue to a fallback backend **carrying the
   same preloaded context block** (`marshal_build_preload_context`) so the fallback
   pays zero re-exploration. Fits the delegation/ensemble layer. **Plan-Search.**

8. **Regression-tracked token-savings benchmark.** A frozen prompt suite run
   "KB-on vs KB-off," capturing real tokens/turns/cost/quality, so KB value is
   provable and guarded as the curator pipeline evolves. Extends `bench/` +
   `docs/BENCHMARKS.md` rather than asserting savings. **Evaluate-Optimize.**

9. **Domain context "packs" + handshake discipline.** Split context into stable /
   session / per-domain packs and request *only the missing pack, one scoped
   question at a time* — never "full context." A retrieval-granularity idea layered
   on Aimee's memory tiers. Lower priority; overlaps existing memory scoping.
   **Recall.**

### Recommended adoption order

**#5 first** (it closes the one clean gap and directly attacks Aimee's own
documented failure mode — the delegate that "elapsed 15 turns with no Write, stuck
in discovery"). Then the cheap Tier-B refinements **#1–#3** (they mostly wire
existing signals together). Then **#6–#8** at the session/delegation/measurement
layers. **#8 should run in parallel** — it's how we'd prove #5 and #1–#3 help.

---

## Part II — Implementation plan: the binding context-contract (item #5)

### Goal

When a delegate (or any agent turn) starts a task, the memory assembler returns a
`retrieval_contract` alongside the preloaded context: the recommended files/symbols
*and* a hard exploration budget derived from confidence. The agent policy binds it;
the attention guard enforces a **server-clamped** cap. High confidence ⇒ raw
recursive scanning is redirected to the index (not blocked — indexed exploration
still works).

### §1 Verified surface inventory (reuse classification)

Confirmed against the tree, with signatures, so the plan reuses real surfaces:

| Surface (signature) | What it provides today | Reuse verdict |
| --- | --- | --- |
| `void retrieval_plan_for_intent(task_intent_t, retrieval_plan_t *)` (`memory_context.c`) | per-intent `kind_budget[]`, `recency_weight`, `include_l3` | **reuse + extend** struct with two int caps |
| `void memory_retrieval_confidence(const char **terms, int, const void *cands, int, double thr, retrieval_confidence_t *)` (`memory_context.c`) | continuous `score = 0.6·coverage + 0.4·separation`, `below_threshold`; **no discrete levels** | **reuse `score`**; level+cap mapping is **new (small) work** |
| `static char *marshal_build_preload_context(const rpc_opts_t *)` (`cli_v1_routes.c`) | builds the preload block from `--files/--context/...` | **reuse + extend** to append the contract block |
| `static void marshal_preload_append_symbol(char *, size_t, size_t *, const char *)` (`cli_v1_routes.c`) | resolves a symbol via `aimee index find`, appends an excerpt (POSIX-only, `popen`) | **reuse**; exact-span is Tier-B #1 |
| `static int attn_config_ingress_max_raw_scans(void)` + enforcement at the `ATTN_OP_RAW_SCAN` branch of `cli_attention_guard.c` (uses `attn_raw_scan_count(arr, now_ts)`) | a **static, session-wide** raw-scan cap read from `aimee.yaml`; on hit, **redirects** to `find_symbol`/`ast_grep_search`/`search_graph`/`get_context_block` (exit 2), does **not** hard-block edits | **reuse as the enforcement point + trust ceiling**; a per-task override source is **new control-plane machinery** (see §4) |
| `tool_condense.c` realized-savings counters | per-turn token telemetry | **reuse** for §6 |

**Honest scoping correction:** `ingress_max_raw_scans` is currently a static
config scalar (`config_t.ingress_max_raw_scans`, default 0), counted **session-
wide**. Driving it **per task/turn** from a contract is therefore *new machinery*,
not free reuse — scoped explicitly in §4.

### §2 Formal contract schema + confidence→caps mapping

Add to `memory_context.c` a sibling of `retrieval_plan_t`:

```c
typedef enum { RC_CONF_HIGH, RC_CONF_MEDIUM, RC_CONF_LOW } rc_level_t;

typedef struct {
   rc_level_t level;
   int  max_supplementary_scans;   /* raw grep/find budget this task (guard-enforced) */
   int  max_supplementary_files;   /* extra reads beyond recommended_files (advisory) */
   int  min_scans_floor;           /* starvation floor; caps never clamp below this */
   task_intent_t intent;           /* carried from retrieval_plan_for_intent */
} retrieval_contract_t;
```

**Precise mapping** from the existing continuous `score` (a pure function, no new
model — a bucketing of a number Aimee already computes). Thresholds are the
proposed defaults, config-overridable (§7 nice-to-haves):

| `retrieval_confidence_t.score` | `level` | `max_supplementary_scans` | `max_supplementary_files` |
| --- | --- | --- | --- |
| `score ≥ 0.70` | `RC_CONF_HIGH` | 0 (redirect-only) | 0 |
| `0.35 ≤ score < 0.70` | `RC_CONF_MEDIUM` | 1 | 1 |
| `score < 0.35` (`below_threshold`) | `RC_CONF_LOW` | 2 | 2 |

`0.35` reuses the scorer's existing `eff_threshold` default so the LOW boundary is
not a new magic number. `min_scans_floor` defaults to 0 **because cap=0 redirects
rather than blocks** (see §5 starvation).

### §3 Emit the contract into the preload block

In `marshal_build_preload_context`, when preload is retrieval-driven (not just
operator `--files`), append a fenced, machine-parseable block after the code:

```
## Retrieval contract
confidence: high            # score=0.81
recommended_files:
  - src/auth.c::handle_login
  - src/session.c
max_supplementary_scans: 0
max_supplementary_files: 0
policy: at confidence=high, do NOT raw-grep; read recommended_files and use the
        indexed tools (find_symbol, search_graph) if you must widen scope.
```

`recommended_files` emits `file::symbol` where a symbol resolved (reusing exact
spans from Tier-B #1), else bare paths. Additive to the existing block; operator
`--files`/`--context` paths are unchanged.

### §4 Enforcement path (explicit new machinery + trust ceiling)

The prompt block in §3 is **advisory**. The *enforced* cap is the attention guard.
Concretely:

1. **New control-plane (scoped honestly):** at contract time, the server writes the
   task's `max_supplementary_scans` into a per-session sidecar next to the existing
   attention log (`$AIMEE_HOME/.cache/attention/<session>.contract.json`, with a
   turn/task id). This is the new piece — a small writer + a read in the guard.
2. **Guard change:** at the `ATTN_OP_RAW_SCAN` branch, compute the effective cap as
   `min(config ingress_max_raw_scans_ceiling, contract max_supplementary_scans)`
   when a live contract sidecar exists, else fall back to today's static behavior.
3. **Trust ceiling (answers "intent-based DoS"/escalation):** the contract can only
   **tighten**, never loosen. The operator-configured static
   `ingress_max_raw_scans` (renamed conceptually to a *ceiling*) is the hard upper
   bound the guard clamps to; a malformed or adversarial contract value can lower
   the budget but can never raise it above the operator ceiling, and is clamped to
   `[min_scans_floor, ceiling]` on read. No client input can widen the cap.
4. **Counter scope:** reuse `attn_raw_scan_count`, but scope the comparison to the
   current task id in the sidecar so the budget is per-task, not only session-wide.

This is deliberately small: one sidecar file, one clamp expression, one scope
predicate — no new IPC, no new enforcement surface beyond the guard that already
runs on every `PreToolUse`.

### §5 Starvation defense (cap=0 cannot wedge the agent)

Three independent guarantees, in priority order:

1. **Redirect, not block.** The existing guard on a raw-scan-cap hit **redirects to
   indexed tools** (exit 2 with a message pointing at `find_symbol`,
   `ast_grep_search`, `search_graph`, `get_context_block`) — it does not block
   edits or indexed exploration. So `cap=0` means "explore through the index," not
   "cannot discover you were wrong." This is verified current behavior, not a
   promise.
2. **Confidence decay / forced re-retrieval.** Retrieval runs every turn, so a wrong
   high-confidence classification is re-scored next turn on new context. Add: after
   `K` consecutive turns (default 2) that hit the cap **with no successful edit**
   (the "stuck in discovery" signal, read from the same attention log), decay one
   confidence level → raise the cap. This directly inverts Aimee's documented
   discovery-spiral failure into an escape hatch.
3. **Floor.** `min_scans_floor` (default 0; operator may set ≥1) guarantees a
   non-zero raw budget regardless of confidence when an operator wants belt-and-
   braces.

### §6 Delegate failure & recovery paths

| Condition | Behavior |
| --- | --- |
| Contract emission fails / assembler errors | **fail-open**: no sidecar written → guard uses today's static config. Never blocks work on contract failure. |
| Delegate call times out / errors | contract sidecar is TTL'd (expires with the session); a dead delegate leaves no stale cap. Failover (survey #7) re-issues with the same preload, same contract. |
| Contract scope insufficient ("contextual blindness") | the §5.2 decay path raises the cap; additionally the delegate may call the indexed tools freely at any confidence (never capped). |
| Delegate requested scope conflicts with cap | cap wins (tighten-only), but §5.2 decay + §5.3 floor bound the downside; the redirect message tells the agent which indexed tool to use instead. |

### §7 Observability & configuration

- **Telemetry (nice-to-have #8):** per-turn structured event via `tool_condense.c`'s
  existing realized-savings counters — `{intent, confidence_score, level, cap,
  scans_used, extra_files_read, cap_hit, decayed}`. Feeds #8's benchmark so token
  deltas attribute to contract adherence.
- **Config ownership (nice-to-have #9):** `ingress_max_raw_scans` (the ceiling) and
  the three score thresholds live in `aimee.yaml` under a `retrieval_contract`
  block, documented in `docs/SETTINGS.md`; owned by the same operator surface as
  the curator flags (`#1170`).
- **Migration / kill-switch (nice-to-have #10):** legacy clients / thin-clients with
  no sidecar and no `aimee.yaml` see today's behavior unchanged (contract absent ⇒
  static path ⇒ inert when ceiling is 0). A single `retrieval_contract.enabled:
  false` reverts to the static `ingress_max_raw_scans` path.

### §8 Executable acceptance criteria (pass/fail, runnable)

1. **High-confidence redirect.** Seed a task whose top candidate scores ≥0.70;
   assert the preload contains `confidence: high` + `max_supplementary_scans: 0`,
   and that a planted raw `grep` in that turn exits 2 with the indexed-tool redirect
   message (drive end-to-end via the guard, not a unit stub). **Fail** if the grep
   is allowed or an edit is blocked.
2. **Medium budget exactness.** Task scoring in `[0.35, 0.70)` permits **exactly 1**
   raw scan, then redirects the 2nd. Assert `scans_used == 1` at redirect.
3. **Clamp / no-escalation.** With `aimee.yaml` ceiling = 1, inject a contract
   sidecar claiming `max_supplementary_scans: 99`; assert the effective cap is 1
   (clamped), proving a contract cannot widen beyond the operator ceiling.
4. **Starvation escape.** Force `RC_CONF_HIGH` on a task the recommended files
   cannot satisfy; assert that after `K=2` capped no-edit turns the level decays and
   the cap rises (agent is never permanently wedged).
5. **Fail-open.** Simulate contract-emission failure; assert work proceeds under the
   static path with no cap regression.

*Until items 1–5 run on a real delegate backend, this proposal is reported as
validation-pending, not done.*

### Non-goals

- Not a new graph or ranker — Aimee's KB stands. This surfaces + enforces what the
  assembler already computes, plus one small per-task override sidecar.
- Not auto-mutating operator-specified `--files`/`--context` preloads.
- Not raising any cap above the operator-configured ceiling (tighten-only).
- Not the proprietary engine, the launcher shims, or the multi-tool wrappers.

## Open questions

- Should the contract govern the primary interactive session too, or delegates
  only? (Recommend **delegate-first** — smaller blast radius, and it targets the
  observed discovery-spiral failure directly.)
- Should `K` (decay turns) and the score thresholds be global config or per-intent
  (a DEBUG task may warrant a looser LOW cap than a REVIEW task)?
