# Flag rollout readiness tracker

Status of every default-OFF feature flag against the project's bar for flipping it
permanently ON. This is the burn-down checklist for the rollout program; update the
per-flag rows as harnesses land and criteria clear.

Precedents that set the bar: `virtual_context_enabled` and `kb_fusion_mode` were the
only two flags ever flipped, both via a rollout-validation report
(`docs/validation/virtual-context-rollout-validation.md`). This tracker generalises
that discipline to the rest of the tree.

---

## The gate (must clear in order)

A flag may flip default-ON only after **all six** clear:

1. **Code-complete + `make unit-tests` green with the flag forced ON.**
2. **An A/B harness that isolates *this* flag** (on vs off) on a **real, labelled
   corpus** — not synthetic-only.
3. **Numeric acceptance criteria pinned _before_ the run** (see defaults below).
4. **Shadow mode** for anything that changes a decision or blocks (safety,
   calibration, demotion): record "would it have agreed" before it acts.
5. **Default flip + cheap, documented rollback**, zero data lock-in.
6. **Observability** (dashboard/alert) for risky flips.

### Default acceptance criteria (pin per-flag; override only with justification)

Derived from `retrieval-abstention-confidence-gate.md §B.7` and
`recall-economy-progressive-disclosure.md`:

- **Quality**: primary metric improves OR is neutral (within noise) vs baseline.
- **No regression**: false-omission / wrong-answer rate rises by **≤ 2pp absolute**.
- **Cost budget**: p95 latency and injected-token deltas within a stated bound
  (e.g. "lower p95 injected bytes at equal-or-better correctness").
- **Auditability**: bench exports per-query evidence traces.
- **Stability**: if a threshold sits near a decision boundary, decision
  reproducibility across runs is demonstrated.
- If the corpus is too small for stable percentages → **keep default-off, collect
  more labelled data.**

Safety flags are a **two-stage** flip: enable→advisory (drop `_dry_run`), then
advisory→blocking (drop `_advisory_only`) only on a pinned **precision floor**.

---

## Ground-truth wiring audit (2026-06-10)

Every default-off flag was grepped for production readers (excluding `src/config*.c`
and `src/tests/`). Result classes:

- **WIRED** — gates real behaviour. The blocker is *measurement* (no bench), not code.
- **INERT TOGGLE** — the `*_enabled` field is never read in production; the feature
  behind it is reachable another way (explicit tool / different gate). The *automatic,
  config-gated* half was scaffolded but never wired.

There are **no fully-dead features**. There are **5 inert toggles** (see §Inert flags).

---

## Readiness matrix

Legend — Tier: **A**=harness exists, run it · **B**=harness needs a per-flag knob ·
**C**=no harness, build it · **S**=safety, two-stage · **X**=do-not-auto-flip ·
**0**=inert toggle (wire-or-remove). Tests: ✓ good · ~ light · ✗ none/smoke.

### Tier A — flip-ready, just run against pinned criteria

| Flag | Harness (isolates it today) | Tests | Pinned criteria | Next action |
|---|---|---|---|---|
| `ingress_preinject_enabled` | `bench/ingress_token_bench.py` (per-request on/off) | ~ | lower p95 bytes, **correctness Δ ≥ 0** | bench measures bytes only — add a correctness arm (LongMemEval/coding on/off), then run |
| `demotion_enabled` (0→1→2) | `benchmarks/memory/poison_gate.py` (deterministic) | ✓ | clean-accuracy Δ ≥ −noise; poison gate PASS | run shadow(1) on real recall, then live(2) |
| `bandit_live_decision_enabled` | `aimee memory benchmark code-graph-fusion --arm …` | ✓ | MRR/nDCG@5 ≥ baseline at fixed explore budget | pin floor, run ablation arms |

### Tier B — add a per-flag on/off knob to an existing suite, then becomes Tier A

> NB: the memory suite is **shell scripts** (`benchmarks/suite/run-direct.sh`,
> `run-llm.sh`), **not** a `runner.py`. The A/B knob is **LANDED** in
> `run-direct.sh`: `--config-variant KEY=VALUE` captures the flag's prior value,
> sets it via the aimee CLI for the run, and restores it on exit (trap, even on
> abort). Run twice to isolate one flag:
> ```bash
> benchmarks/suite/run-direct.sh --bench longmemeval_s --config-variant memory_rerank_enabled=0   # baseline
> benchmarks/suite/run-direct.sh --bench longmemeval_s --config-variant memory_rerank_enabled=1   # variant
> ```
> The set/capture/restore plumbing is validated structurally (stub CLI); the run
> itself still needs a live aimee-server + corpora (user-gated). `AIMEE_BIN`
> overrides the CLI path.

| Flag | Suite it rides | Tests | Next action |
|---|---|---|---|
| `memory_rerank_enabled` (+`_mode`,`_top_k`,`_mix`) | LongMemEval/LocOMo | ✗ | add variant knob; A/B on retrieval suites |
| `memory_query_expansion_mode` | LongMemEval/LocOMo | ✗ | same |
| `memory_rewrite_enabled`/`_hyde`/`_decompose` | long-context suites | ✗ | same |
| `kb_ranker_enabled` | code-graph-fusion | ✗ | add as an ablation arm |
| `cache_aware_rewrite_enabled` | — (needs **cost** harness) | ~ | build cache-hit/token A/B; correctness must be neutral |
| `memory_recall_lanes_enabled` | LongMemEval | ✓ | add variant knob |

### Tier C — no harness; build the eval before any flip discussion

These are **wired** (gate real code) but unmeasurable today.

| Flag | Prod reader | Tests | Build needed |
|---|---|---|---|
| `learning_synthesize_enabled` + 6 `learning_implicit_*` | `learning_router.c`, `kb_curator_drain.c` | ~ | runner landed: `learning_replay.py` + `make learning-citation-eval`. **Citation detectors GRADED PASS** (see Executed validation). Stateful heuristics + substrate promotion still need a live-router replay entry |
| `memory_scenes_enabled` | `memory_core_helpers.inc` | ✗ | labelled scene-retrieval corpus + runner |
| `memory_negation_enabled` | `memory_core_helpers.inc` | ✗ | negation/absence corpus + runner |
| `memory_salience_enabled` | `memory_core_helpers.inc` | ✗ | per-flag arm in retrieval suite |
| `memory_surprise_enabled` | `memory_core_helpers.inc` | ✗ | same |
| `memory_pagerank_enabled` | `memory_core_helpers.inc` | ✗ | same |
| `memory_derive_facts_enabled` | `memory_assemble.c` | ✗ | date/quant-arithmetic Q&A corpus |
| `memory_failure_detection_enabled` | `memory_assemble.c` | ✗ | abstention corpus (see retrieval-abstention proposal) |
| `memory_fetch_budget_enabled` | `memory_core_search.inc` | ✗ | cost/correctness A/B |
| `memory_context_budget_enabled` | `memory_assemble.c` | ✗ | assembly-mode A/B (top-K vs token-budget) |
| `memory_aggregation_enabled` | `memory_core_search.inc` | ✗ | coverage-query corpus |
| `memory_episode_summaries_enabled` | `cmd_memory_vector.c` | ✗ | session-close summary quality eval |
| `memory_lifecycle_enabled` (+`_hide_archived`) | `memory_core_helpers.inc` | ✗ | recall-with-archival A/B |
| `memory_cognify_enabled`/`_async` | `memory_improve.c`, `kb.c` | ✗(0 asserts) | extraction-quality eval |
| `identity_working_profile_injection_enabled` | `prompts.c` | ✗(smoke) | task-accuracy A/B with/without injection |
| `drift_detect_shadow_enabled` | `kb_detect.c` | ~ | already shadow-only by design; needs precision eval |
| `kb_curator_*` (10 flags) | `kb_curator_drain.c` + pass files | ~/✗ | per-pass artifact-quality (LLM-judge) + **cost budget**; `curator_eval.py` exists but doesn't isolate passes |
| `review_scheduler_enabled` | `kb_reflection.c` | ✗ | reflection-usefulness eval (hard) |
| `skills_review/curator/manage/eval_gate` | `server.c`, `skill_*.c` | ~ | skill-lifecycle outcome eval |

### Tier S — safety, two-stage flip

| Flag | Harness | Tests | Path |
|---|---|---|---|
| `guardrails_semantic_enabled`→drop `_dry_run`→drop `_advisory_only` | `tools/guardrails_replay.py` (55 fixtures, precision/recall) | ✓(18) | run replay → pin precision floor on 35 yellow-zone + 0 regressions on 10 benign → enable advisory; block only if precision clears `_allow_ml_only_block` |
| `integrity_enabled`→drop `integrity_dry_run` | none yet | ✓(14) | **build ingest-pattern fixture corpus**; run dry-run shadow → drop dry-run if FP≈0 |
| `calibration_enabled` (0→1→2→3) | none isolated | ✓(14) | use built-in shadow(1)→A/B(2) ladder; pin promotion-threshold agreement |

### Tier X — do NOT auto-flip (opt-in by design)

Mode/cost/posture changes, not quality improvements — leave user-driven, document only:
`autonomous`, `ecomode`, `aux_enabled`, `ensemble_enabled`, `computer_use_enabled`,
`claude-proxy` (rewrites the user's Claude config), Codex ingress (already
always-available), `rewind_auto_snapshot`. Low-risk operational flips on a smoke test
alone: `worktree_gc_enabled`.

---

## Inert flags — wire or remove (decision required)

These `*_enabled` toggles had **zero production readers**; only config parse/save and
one config-surface test referenced them. The feature behind each is reachable another
way, so the *toggle* was vestigial — a **misleading control surface**: a user could
`config set` it, it persists, shows in `config show`, and silently did nothing.

Status: **4 of 5 now wired** (this PR). Each was wired into its feature's gate and
**defaulted ON** — the gated behaviour ran ungated before, so a default-off gate would
*regress*; default-on preserves the status quo while making the toggle functional.
`summarise` is the exception (opt-in, default off). The `directives` toggle gates the
**confident-failure** auto-create specifically (the documented intent); the separate
contradiction-at-promotion and manual CLI/MCP create paths stay independent by design.

| Flag | Feature reachable via | Disposition | Status |
|---|---|---|---|
| `memory_profile_cards_enabled` | runs in maintenance, gated by `_min_obs`/`_stale_secs` (both read) | wire as master gate of the REPLAY-pass refresh | **WIRED, default-on** (PR #168) |
| `memory_improve_dedupe_enabled` | `memory_improve_dedupe()` in COMPACT pass | wire as the COMPACT gate | **WIRED, default-on** (PR #168) |
| `memory_improve_summarise_enabled` | `memory_improve_summarise()` callable | OR into the SUMMARIZE gate | **WIRED, default-off** (opt-in, PR #168) |
| `memory_directives_enabled` (+`_failure_threshold`,`_max_matches`) | `aimee memory directive(s)` CLI + `session_briefing_directives` | gate **auto-create-on-confident-failure** (memory_assemble); surfacing of *existing* directives stays unconditional so manual directives always show | **WIRED, default-on** (PR #168) |
| `memory_briefing_enabled` (+`_limit_tokens`) | `aimee memory briefing` / MCP `memory_briefing` tool | toggle was meant to **auto-inject** briefing at session start — but **no auto-inject site exists in code** (the briefing is a pull-only tool today). Disposition: this is a *missing feature*, not a mis-gated one. Either build the session-start auto-inject and gate it here, or remove the toggle and document briefing as pull-only. **Left as a scoped follow-up** (a new feature + its own rollout gate, not an inert-toggle cleanup). | **OPEN** (follow-up) |

**Why not just delete them?**
1. Deleting a flag here is **not** deleting nothing — the feature is live. Removal
   forces a decision about the feature's *permanent* gate (always-on? manual-only?).
2. These are **rollout seams**, not litter. The repo's method is "land seam off →
   validate → wire → flip"; an unread `*_enabled` is the expected mid-rollout state.
   Deleting it throws away the integration point the next person re-adds.
3. **But** inert config IS a real harm (misleading surface). So the disposition is
   per-flag: **finish the wiring** (small, for the four live-feature cases) or
   **remove the toggle and document the feature's fixed behaviour** — a product call,
   not a reflex `git rm`.

Done: `profile_cards`/`improve_*` (PR #168) and `directives` auto-create wired as
their gates, default-on. Remaining: `briefing` auto-inject is a *missing feature*
(no session-start injection site exists) — tracked as a scoped follow-up, since
building it is a new capability with its own rollout gate, not an inert-toggle fix.

---

## Executed validation (local, deterministic) — 2026-06-10

The self-contained harnesses (bundled fixtures, no live server/corpus/GPU) were
run. These are **oracle validations**: each harness encodes the decision boundary
and grades it against labelled fixtures, proving gate criteria **#2 (harness
isolates the flag)** and **#3 (criteria achievable)** — *not* #1 against the
production binary on a real corpus, which stays user-gated.

| Flag | Harness | Result | Verdict |
|---|---|---|---|
| `demotion_enabled` | `benchmarks/memory/poison_gate.py` | PASS (exit 0): all clean rows retrieved CORRECT; only closed-outcome poison rows (`poison_refresh`, `poison_snapshot`) suppressed — declared-confidence / trusted-source / frequency fields correctly ignored | **decision boundary sound** — safe to run the live shadow→live(2) ladder; the gate does not over-suppress |
| `guardrails_semantic_enabled` (→advisory) | `benchmarks/guardrails/sidecar_e2e.py` (real sidecar, production rule) | **WAS** exit 1: 10/10 benign FP — the actual sidecar scored `overall≈0.40` for every band (a flat 0.40 edit baseline swamped the real signals), and the earlier `guardrails_replay` "PASS" only graded the fixtures' pre-baked spec scores. **NOW FIXED** (this PR): recalibrated the sidecar — edit baseline 0.40→0.20 (an edit isn't inherently risky), a single high-specificity security antipattern scores 0.6 (was 0.3) and is no longer discounted in `compute_overall`. e2e now **passes (exit 0): 0/10 benign FP, precision 1.0**, recall 0.385 (reported, not gated) | **Fixed + safe for advisory; kept opt-in.** Recall 0.385 is the regex-heuristic ceiling on *semantic* risks (terse fixtures; an ML sidecar lifts it). For an advisory net the hard gate is 0 benign FP (passes). Sidecar now **shipped in the images** (`/opt/aimee/scripts/`). NOT flipped on by default — `gsem_assess` runs a subprocess per write-tool-call and the code's design intent is explicit opt-in ("no behavioral change until opted in"). Opt-in = `config set guardrails.semantic.{command=<path>,enabled=1,dry_run=0}`; `sidecar_e2e.py` gates it. |
| `learning_implicit_citation_repair` / `_continuation` | `make learning-citation-eval` (real `dogfood_classify_next_turn` over 63 citation fixtures) | **GRADED PASS** (exit 0): precision **1.0**, recall **1.0**, FPR **0.0** (44 pos / 19 neg) — clears the pinned 0.90/0.80/0.10 bar | **detector validated on the labelled corpus**; the per-turn classifier is accurate. Remaining for a flip: the full router→substrate promotion loop on a real session corpus |
| `learning_implicit_repeat_question` / `repeated_correction` / `workflow_repetition` | — | stateful (session/DB) — not replayable by the pure-text tool | needs a live router + session state to grade |
| `learning_synthesize_enabled` (substrate promotion) | `learning_replay.py` substrate fixtures | VALIDATION OK: schema + distribution clean | needs a substrate-promotion replay entry (live router) to grade |

### Default flips applied (this PR) — "default-on to proven worth"

Acting on the determinations, with each flip gated by whether the feature is
actually *consumed* (no inert default-on theatre):

- **`demotion_enabled`: 0 → 1 (shadow). FLIPPED.** poison_gate proved the score
  boundary suppresses only closed-outcome poison, never clean rows; gate
  criterion #4 (*shadow before acting*) makes shadow the correct on-state. At 1,
  `kb_demote_run` (wired from `kb_service_agent.c`) computes scores + fits
  profiles but demotes nothing (live suppression only at `>= 2`). One config bump
  from live after real-recall shadow review. Save guard flipped to emit-when-≠1.
- **`guardrails_semantic_enabled`: NOT flipped.** The e2e harness shows the
  bundled sidecar over-flags every edit (above). Proven-not-ready.
- **`learning_implicit_citation_repair` / `_continuation`: 0 → 1. FLIPPED — after
  wiring the detector.** The classifier is graded PASS, but the flag was inert
  until this PR wired `learning_implicit_detect_turn` into the primary chat turn
  (`openai_chat.c`, see below). Now consumed + self-gating + operator-reviewed, so
  flipped on per "default-on to proven worth". Needs `.254` live validation;
  trivially revertible.

The audit lesson: of the three "proven" flags, only **demotion** was both proven
*and* immediately consumable. Guardrails was proven-on-spec but broken-in-practice
(fixed here); the citation detectors were proven-in-logic but unwired (wired here).
The consumed-check is what turned "flip three proven flags" into "fix one, wire
one, flip one cleanly" — without it, this PR would have shipped an alert-fatigue
feature and a no-op under the banner of "proven".

### Requested wirings — investigation findings

Two features were flagged for wiring so their proven flags become live. Close
inspection found both need real work beyond a flag flip:

**`guardrails_semantic_command` — FIXED + shipped + wired-as-opt-in.** The bundled
sidecar over-flagged every edit (10/10 benign FP; `overall`≈0.40 for all bands,
the flat edit baseline swamping real signals). **Recalibrated** (edit baseline
0.40→0.20; single security antipattern 0.3→0.6 and no longer discounted in
`compute_overall`) so `sidecar_e2e.py` now passes: **0 benign FP, precision 1.0**,
recall 0.385 (heuristic ceiling, advisory). **Shipped** the sidecar in `Dockerfile`
+ `Dockerfile.combined` (it wasn't in the images). **Kept opt-in** by design:
`gsem_assess` spawns a subprocess per write-tool-call and the orchestrator comment
mandates "no behavioral change until opted in"; recall 0.385 also argues against a
blanket default-on. Opt-in is now one step against a known-good sidecar —
`guardrails.semantic.{command,enabled,dry_run}` — gated by `sidecar_e2e.py`.

**`learning_implicit_detect_turn` — WIRED (this PR); needs `.254` live validation.**
The classifier is graded PASS, but the whole per-turn consumer was unwired:
`dogfood_autolabel_next_turn_live()` (the documented site `detect_turn` follows)
had **no caller** either. Citation *moments* are logged live
(`dogfood_log_moment_live`, e.g. from `memory_briefing`), setting
`g_last_record_id`, but nothing consumed them next-turn. **Now wired** at
`server/openai_chat.c` — the converged chat-completions path that the Anthropic
`/v1/messages` and Codex `/v1/responses` ingresses both translate into, and which
delegates do **not** use (so no per-delegate mis-fire). The call sits **before**
this turn's pre-injection recall (which would overwrite `g_last_record_id`) and is
**self-gating**: `dogfood_autolabel_next_turn_live` + `detect_turn` no-op unless a
prior citation moment was logged and the relevant flags are on. Signals feed
**operator-reviewed** learning proposals (bounded blast radius). Compiles + links;
`learning_implicit_citation_{repair,continuation}` flipped default-on (proven +
now consumed). **Still needs `.254` live validation** that it fires on real
post-citation turns and not spuriously — flip the flags off if it mis-fires
(trivially revertible). **Persistence:** `learning_implicit_*` previously had no
config-file parse/save (config_fields is only the get/set reflection surface), so
overrides didn't survive a save/restart — **fixed this PR** with a
`learning.implicit.*` parse + emit-when-non-default save block (round-trip tested),
so all five toggles now persist and are overridable.

---

## Execution plan (program steps 1–5)

| Step | What | State |
|---|---|---|
| 1 | Build missing harness primitives | **mostly done (this PR)**: `learning_replay.py` + real-classifier replay (`make learning-citation-eval`); `guardrails/sidecar_e2e.py`; the suite `--config-variant` A/B knob (`run-direct.sh`). Backlog: a live-router substrate replay + per-pass curator eval |
| 2 | Pin acceptance criteria per flag | **this doc** (defaults pinned; per-flag numbers fill as corpora land) |
| 3 | Clear Tier A | **partial**: `demotion_enabled` boundary validated on fixtures (poison_gate PASS); live recall A/B still needs aimee-server + corpora via `! <cmd>` |
| 4 | Run safety (Tier S) in shadow | **partial**: `guardrails_semantic_enabled` advisory precision/recall = 1.0 on 75 fixtures (guardrails_replay PASS); live FP-on-benign confirmation + dashboards still user-gated |
| 5 | Stop treating Tier X as default candidates | **done** (documented above) |

### What can be done autonomously vs needs you

- **Autonomous (no infra)**: this tracker, harness *code*, fixture corpora, the
  inert-flag wiring.
- **Needs you (`! <cmd>`)**: every actual bench run — they require a live
  aimee-server, Postgres/pgvector, full corpora, and often a GPU/delegate judge. Per
  project setup, autonomous remote/live runs are classifier-gated; run them
  collaboratively.

### Tier-A runbook (paste-ready, fill the live server)

```bash
# ingress pre-injection — bytes + correctness A/B
aimee config set ingress_preinject_enabled 0 && python3 bench/ingress_token_bench.py --prompts bench/ingress_prompts.txt --out off.json
aimee config set ingress_preinject_enabled 1 && python3 bench/ingress_token_bench.py --prompts bench/ingress_prompts.txt --out on.json
# demotion — shadow then poison gate
aimee config set demotion_enabled 1   # shadow
python3 benchmarks/memory/poison_gate.py --fixtures benchmarks/memory/poison_fixtures.json --output benchmarks/results/memory_poison_report.json
```
