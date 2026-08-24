# Memory query classification correctness, fitted route weights, and a retrieval sufficiency gate

- **State:** PENDING (proposed) — four defects and two structural gaps in the memory
  query-planning path, found while auditing `Kitzkatz/memoria` against Aimee's memory module.
  Slice 1 is a live correctness bug with a repo-wide pattern sweep; Slices 2–5 are
  independently gated improvements. Each slice files per the repo's proposal lifecycle on
  completion.
- **Author:** JBailes
- **Date:** 2026-08-21

## Problem

Aimee's memory retrieval classifies every query twice — once into a *route*
(`MEM_ROUTE_LEXICAL / SEMANTIC / GRAPH / HYBRID`) and once into an *intent*
(`MEM_QUERY_GENERAL / TEMPORAL / ENTITY / PROCEDURAL`) — and both classifications steer
ranking. Four things are wrong with how that happens:

1. **The classifiers match substrings, not words.** `"update the candidate ranker"` is
   classified `MEM_QUERY_TEMPORAL` because `"date"` occurs inside `"candidate"`. This is a
   live scoring bug, and the same pattern repeats across at least seven keyword-list sites
   in the memory module.
2. **The route weights are compiled-in constants.** Aimee has learning-to-rank weight
   fitting with a benchmark gate (`kb_ranker_fit.c`), contextual bandits (`kb_bandit.c`),
   and Bayesian threshold calibration (`kb_calibrate.c`). None of them can reach the memory
   router's weights, because those weights are a `switch` statement rather than an artifact.
3. **Candidate collection never decides it has enough.** Every query runs every lane in
   sequence, including an unconditional `LIKE` fallback, regardless of what the earlier
   lanes already returned. The telemetry needed to short-circuit is already collected and
   is not consumed as a control signal.
4. **Nothing diversifies the recall bundle.** Dedup is thorough at write time and absent at
   read time, so near-identical restatements of one fact can consume the whole token budget.

Items 1 and 2 are the load-bearing ones. Item 1 is a bug; item 2 is the reason the router
cannot improve on its own.

## Current state — the trace (all file:line verified on branch `agent/human-trigger-workflows`)

### Classification

`memory_query_intent()` — `src/modules/memory/memory_core_search.c:1004-1033`. A
first-match-wins cascade of raw `strstr` against `raw_query` and `norm_query`, with
unanchored single-word needles:

```c
strstr(raw_query,  "date")  /* ⊂ candidate, update, validate, mandate */
strstr(raw_query,  "ago")   /* ⊂ Chicago, agony */
strstr(norm_query, "day")   /* ⊂ birthday, everyday, Monday */
strstr(norm_query, "time")  /* ⊂ sometimes, runtime, timeout */
strstr(norm_query, "year")  /* ⊂ yearly */
strstr(norm_query, "setup") /* ⊂ setups */
```

**A whole-token matcher already exists in the same translation unit, 2 lines below this
function.** `memory_query_has_term()` — `memory_core_search.c:1035` — is
`memory_token_in_norm(norm_query, term)`, and `memory_token_in_norm()`
(`memory_core_search_b.c:339-354`) is a correct space-delimited whole-token/phrase matcher.
The three functions immediately following the classifier —
`memory_query_wants_recent/past/future()`, `memory_core_search.c:1040-1064` — already use it,
over a vocabulary that overlaps the classifier's own (`"today"`, `"recent"`, `"latest"`,
`"before"`, `"after"`, `"earliest"`, `"next"`). The classifier is the outlier, not the norm.

### What the misclassification costs

`memory_unit_kind_intent_boost()` — `src/modules/memory/memory_core_helpers_b.c:942-967`:

| Intent | episodic | semantic | procedural | other |
|---|---|---|---|---|
| `TEMPORAL` | **+0.18** | −0.06 | — | — |
| `ENTITY` / `GENERAL` | −0.03 | **+0.12** | — | — |
| `PROCEDURAL` | — | — | **+0.22** | **−0.08** |

Intent also sets semantic fetch floors in `memory_collect_candidates`
(`memory_core_search_c.c:168-178`). So a misclassified code question is ranked as if it
asked when something happened, and fetches on the temporal budget.

### The same pattern, elsewhere in the module

This is a pattern defect, not a single site. It appears in **three distinct forms**, which
matters because the right fix differs per form:

**Form A — no boundary at all** (bare `strstr` / `strcasestr`). Both edges unchecked.

| Site | Needle | Collides with |
|---|---|---|
| `memory_core_search.c:1004` `memory_query_intent` | `date`, `ago`, `day`, `time` | candidate, Chicago, birthday, sometimes |
| `memory_core_search.c:843` `memory_query_route` | `fixes` | prefixes, suffixes |
| `memory_context.c:210` `quantitative_keywords` | `count`, `total`, `times` | account, totally, sometimes |
| `memory_context.c:158` `temporal_phrases` | `lately`, `recently` | latent — the list is mostly multi-word, which is the only thing holding it up |
| `memory_context.c:126` `session_keywords` | `recap` | recapture |
| `memory_core_scope_embed.c:236` `shared_keywords` | `auth`, `cert` | **author**, **certain** — both very common; mis-tags ordinary memories as shared infrastructure |
| `memory_advanced.c:507` `match_keywords` | `structured` | **`unstructured`** — the positive style marker is a substring of the negative one, so text complaining about unstructured output scores *both* sides of the dimension |
| `memory_lifecycle.c:77,94` date/relative markers | — | none found; converted for uniformity |

**Explicitly excluded: `memory_assemble.c:557` `compute_term_overlap`.** It has the same
shape but a materially different contract, and converting it is a regression. Its needles are
**user query terms**, not a closed vocabulary, and the substring match is doing duty as a
poor-man's stemmer: `"cert"` must cover `"certificate"`, `"auth"` must cover
`"authentication"`. `test_context_assembly.c:91` pins exactly that — a query of
`"fix PostgreSQL cert auth"` must rank `"authentication flow validates certificate chain"`
above an unrelated deploy memory. Whole-word matching drops that overlap to zero and the test
fails. The cost of leaving it is that `"add"` also counts against `"address"`; removing that
without losing the stemming needs a real stemmer, which is separate work. A closed keyword
list has no such tension, which is why every site above converts and this one does not.

**Form B — left boundary only, right boundary missing.** `count_keyword_matches()`
(`memory_context.c:129`) *does* check `p == text || !isalpha(p[-1])`, so suffix collisions
(`ship` in `relationship`, `times` in `sometimes`) were never live here. The live defect is
the unchecked right edge:

| List | Needle | Matches |
|---|---|---|
| `plan_keywords` | `add`, `new` | **address**, padding·no, **newsletter**, newer |
| `debug_keywords` | `fix` | **fixture** |
| `review_keywords` | `check` | **checkout** |

This form must keep prefix tolerance — `deploy`/`deployed`, `release`/`released`,
`build`/`building` are the same word and should still match — so a strict whole-token test
would trade one defect for a recall regression. The fix admits a common inflectional suffix
(`s`, `es`, `ed`, `d`, `ing`) and rejects any other letter continuation.

**Form C — awareness without the fix.** `memory_context.c:157` carries the comment *"Single-word
markers must match as whole tokens to avoid spurious hits (e.g. 'since' inside 'business')"*,
but the mitigation applied was to make the list mostly multi-word phrases, not to use a
matcher. The comment describes a rule the code does not enforce.

Concrete impact outside the classifier: `count_keyword_matches()` feeds the recall-bundle kind
budgets at `memory_context.c:509-540`. `"fix the address parsing"` scores `debug+1` **and**
`plan+1` — the `plan` point comes entirely from `"add"` inside `"address"`.

### Route weights

`memory_query_plan()` — `memory_core_search.c:918-976`. Four routes × four weights, plus six
query-shape adjustments, all literals:

```c
case MEM_ROUTE_SEMANTIC:
   out->weights.lexical_weight  = 0.45;
   out->weights.semantic_weight = 1.00;
   out->weights.graph_weight    = 0.20;
   out->weights.temporal_weight = 0.35;
   break;
...
case MEM_SHAPE_WHEN:
   out->weights.temporal_weight += 0.55;
```

Fitting infrastructure that exists and cannot reach them:

- `src/kb/kb_ranker_fit.c` — reads a joined feature/outcome view, runs `scripts/rank-fit.py`,
  gates on NDCG@k lift over the incumbent (`RANK_FIT_LIFT_EPSILON` 1e-3,
  `RANK_FIT_DEFAULT_MIN_GROUPS` 8), and promotes a `ranker_model` artifact only on measured
  lift.
- `src/kb/kb_bandit.c` — contextual arm registration and reward feedback, arms persisted as
  `policy_arm` artifacts.
- `src/kb/kb_calibrate.c` — Bayesian promotion-threshold calibration per surface.

Only `config_memory_semantic_weight()` (`config.h:887`) can influence memory ranking from
outside the binary, and it is a single global scalar applied at
`memory_core_helpers.c:746` — not the per-route table.

### Lane sequence and the telemetry that already exists

`memory_collect_candidates` — `memory_core_search_c.c:120-256` — runs, unconditionally and in
order: graph (route-gated) → variant → decomposition sub-queries → unit-semantic → semantic →
negation FTS → negation vector → `LIKE` fallback.

Already collected, already unused as a control signal:

- `memory_note_candidate_sources()` maintains a per-candidate `MEM_SOURCE_*` bitmask
  (`MEM_SOURCE_UNIT`, `_SEMANTIC`, `_LEXICAL`, `_LIKE`) in `source_stats`.
- `memory_record_query_stage_metric(plan, "<lane>")` fires as each lane completes.
- `memory_record_query_plan_metrics()` (`memory_core_search.c:860`) already emits
  `memory.query.route.<route>` and `memory.query.route.<route>.latency_ms`.

### Read-time assembly

`recall_truncate_section()` — `memory_context.c:982` — trims the assembled bundle to the token
budget by section priority and score order. Write-time dedup (`memory_improve_dedupe()`,
`db2_memory_active_kind_dedupe_candidates()`, `kb_neardup.c`) has no read-time counterpart.

### Available gates

`benchmarks/memory/` (`bench_dmr.py`, `bench_msc.py`, `bench_ruler.py`, `bench_mrcr.py`,
`bench_l_eval.py`, `poison_gate.py`), `benchmarks/longmemeval/`, `benchmarks/locomo/`,
`benchmarks/rank/kb_hybrid/queries.json`, and `src/tests/test_memory_retrieval_eval.c`.

## Language and placement

Standing direction: only the event bus and its communication surface stay in C; everything
else becomes a Go module. That is already a documented, in-flight program — see
[`db2-as-a-go-module`](./db2-as-a-go-module.md) for the two-step pattern (C library → C module
behind a frozen event contract → pure-Go module proven against it, *"no cgo bridge"*).

Memory retrieval has **not** been ported. `server-go/modules/memory` is ~3.1k lines covering
commands, embedding, extraction, ontology and the PII gate; there is no Go counterpart to
query planning, candidate collection, graph fusion, or bundle assembly (no `QueryPlan`,
`QueryIntent`, or `ExpandFromSeeds` anywhere under `server-go/`). The C path traced above is
the live one.

That splits this proposal cleanly:

- **Slice 1 lands in C, now.** It is a correctness fix to live code. Under the migration
  pattern the Go port is *proven against the C implementation*, so a defect left in C becomes
  the conformance baseline and gets faithfully reproduced. Fixing before the port is strictly
  cheaper than fixing after, and the fix is small and fully covered by deterministic tests.
- **Slices 2–5 are Go work, and should not be built in C.** They add new behaviour —
  scored classification, a fitted weight artifact, a sufficiency predicate, a diversity pass.
  Adding those in C creates debt that the port must then carry across. They belong in
  `server-go/modules/memory` behind the retrieval event contract, and they are gated on that
  contract existing.

The practical ordering consequence: **Slice 1 ships on its own merits and does not wait**;
Slices 2–5 queue behind the memory-retrieval module boundary, and their designs below
describe the behaviour to build, not the language to build it in.

## Target architecture

1. Every keyword classification in the memory module matches **whole tokens or whole
   phrases**, through one shared helper, with no site left on raw `strstr`.
2. Classification produces a **score and a confidence**, not a first-match verdict, so
   ambiguity is observable and thresholdable rather than decided by source order.
3. The route weight table is a **fitted artifact with a compiled-in default**, on the same
   `ranker_model` / benchmark-gate / promotion-epsilon rails as `kb_ranker_fit`.
4. Candidate collection consults a **sufficiency predicate** between lanes, driven by the
   `MEM_SOURCE_*` and stage-metric data already being collected.
5. Read-time assembly applies **diversity** before the token-budget trim.

Items 3 and 5 must not change behaviour until their gates are met: the fitted artifact
starts absent (falling back to today's exact literals) and diversity starts disabled.

## Slices

Each slice: pure core + deterministic tests first, then wire, then gate. Slice 1 is C (live
correctness fix); Slices 2–5 are Go, behind the memory-retrieval module boundary, per
**Language and placement** above.

1. **[C — DONE] Whole-token classification, and the module-wide sweep.**
   Rewrite `memory_query_intent()` to normalize once and match through
   `memory_query_has_term()`, which already sits 2 lines below it. Then sweep every site in
   the table above onto the same helper (or a shared `memory_keyword_hit()` wrapper for the
   `count_keyword_matches` / lowercased-buffer call shape, which matches against a
   lowercased raw buffer rather than a `normalize_key` output and therefore needs its own
   boundary check — punctuation as well as space).
   This slice owns the pattern sweep: `memory_core_search.c` ×2, `memory_context.c` ×5,
   `memory_core_scope_embed.c`, `memory_lifecycle.c` ×2. Verify no site remains with
   `scripts`-side grep for `strstr(` over a keyword-list identifier in
   `src/modules/memory/`.
   **Tests (deterministic, no DB):** `"update the candidate ranker"` → `MEM_QUERY_GENERAL`
   (not `TEMPORAL`); `"what happened on Chicago's rollout"` → `TEMPORAL` via `"what
   happened"`, not via `"ago"`; `"fix the address parsing"` → `debug` outscores `plan`;
   `"prefixes for the embedder"` → not `MEM_ROUTE_GRAPH`. Plus a regression assertion that
   every genuine temporal phrasing already covered still classifies `TEMPORAL`.
   **Gate:** existing memory suites stay green; `test_memory_retrieval_eval` shows no
   regression. Independently shippable — a live bug, must not wait on the rest.

2. **[Go] Scored classification with exclusions and confidence.**
   Replace first-match-wins with an additive score per candidate class: keyword hit `+1.0`,
   exclusion hit `−2.0`, per-class `min_confidence` floor, explicit priority order for ties.
   Emit the winning confidence alongside the existing
   `memory.query.route.*` / `memory.query.shape.*` counters so ambiguous classification
   becomes measurable before anything is tuned on it.
   **Gate:** deterministic classification tests from Slice 1 still pass; new counters visible
   on `GET /v1/dashboard/metrics`. Depends on Slice 1.

3. **[Go] Route weights become a fitted artifact.**
   Extract the `memory_query_plan()` table into a `memory_route_model` artifact
   (route × {lexical, semantic, graph, temporal} plus the shape deltas), loaded through the
   same accessor pattern as `kb_ranker.c`'s `g_weights`, with **today's exact literals as the
   compiled-in default when no artifact is present**. Extend `kb_ranker_fit` to fit this
   surface from the memory retrieval feature/outcome rows, reusing its benchmark gate and
   `RANK_FIT_LIFT_EPSILON` promotion rule unchanged.
   **Gate:** with no artifact present, `memory_query_plan()` output is byte-identical to the
   pre-slice behaviour (deterministic test over all 4 routes × 6 shapes). A fitted model
   promotes only on measured NDCG lift on `benchmarks/memory/` — no promotion without
   evidence, exactly as on the KB path.

4. **[Go] Retrieval sufficiency gate.**
   Add a predicate evaluated between lanes — required sources present, ≥N distinct
   `MEM_SOURCE_*` bits set, ≥M unique candidates — that can retire the `LIKE` fallback and
   the decomposition sub-queries when earlier lanes already cleared the bar. Config-gated,
   **default off**.
   **Gate:** with the gate off, candidate sets are identical to today. With it on,
   `benchmarks/memory/` recall is unchanged within noise and
   `memory.query.route.<route>.latency_ms` drops. Ship only if both hold.

5. **[Go] Read-time diversity before the budget trim.**
   MMR pass in front of `recall_truncate_section()`, λ config-gated, **default off** (λ=1 is
   exactly today's behaviour). Sweep λ on `benchmarks/longmemeval/` and `benchmarks/locomo/`.
   **Gate:** measured lift on at least one suite with no regression on the others, and
   `benchmarks/memory/poison_gate.py` still green — diversity that admits a poisoned
   near-duplicate is worse than none.

## Risks / open questions

- **Slice 1 changes live classification results.** Some queries that were incidentally
  classified `TEMPORAL` by a substring collision may have been getting a helpful episodic
  boost by accident. The regression assertion in Slice 1 covers genuine phrasings; the
  accidental ones are the bug. Expect small movement in
  `memory.query.intent.*` distribution and state it in the PR.
- **`memory_token_in_norm()` is space-delimited.** It is correct against `normalize_key`
  output. The `memory_context.c` sites match against a lowercased *raw* buffer where
  punctuation is still present, so `"ship."` would fail a space-only boundary test. Slice 1
  must handle punctuation boundaries for those sites rather than reusing the norm-only
  helper blindly.
- **Slice 3's training signal.** The KB fitter consumes a joined feature/outcome view; the
  memory path's equivalent outcome labels need to be confirmed to exist at sufficient volume
  before the fit is meaningful. If they do not, Slice 3 lands the artifact plumbing plus
  default fallback and the fit waits — the plumbing is still worth having, because it is what
  makes the weights reachable at all.
- **Slice 4 could mask a lane regression.** If a lane silently stops returning results, a
  sufficiency gate that short-circuits before it would hide the failure. The stage metrics
  must keep firing for skipped lanes with an explicit `skipped` disposition, not go quiet.
- **Ordering.** Slices 1 and 2 are prerequisites for 3 — fitting weights on top of a
  classifier that misroutes would fit the wrong thing.

## Acceptance

- No site in `src/modules/memory/` classifies a query by unanchored `strstr` against a
  keyword list; all go through a whole-token/phrase matcher.
- `"update the candidate ranker"` classifies `MEM_QUERY_GENERAL`.
- Classification emits a confidence value observable on the metrics dashboard.
- `memory_query_plan()` reads its weights from a `memory_route_model` artifact, falling back
  to byte-identical compiled-in defaults when absent, and `kb_ranker_fit` can promote a
  fitted model for it under the existing lift gate.
- A sufficiency predicate exists between collection lanes, is default-off, and demonstrably
  reduces route latency without reducing benchmark recall when enabled.
- An MMR pass exists in front of the recall-bundle trim, is default-off, and does not
  regress `poison_gate.py`.

```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_memory_query_classification"}
- {id: 2, tier: mechanical, check: "! grep -rnE 'strstr\\((lower|buf|norm_query|raw_query)[^)]*(keywords|phrases|markers)\\[' src/modules/memory/"}
- {id: 3, tier: mechanical, check: "make unit-tests TEST=test_memory_route_model_default_parity"}
- {id: 4, tier: mechanical, check: "make unit-tests TEST=test_memory_retrieval_eval"}
- {id: 5, tier: integration, check: "curl -sk $AIMEE_API/v1/dashboard/metrics | jq -e '.memory.query.intent_confidence != null'"}
- {id: 6, tier: integration, check: "python benchmarks/memory/poison_gate.py"}
```
