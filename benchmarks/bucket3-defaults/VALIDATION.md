# Bucket-3 default-flag validation recipes

These are the experimental feature flags that the default-on review put in **Bucket 3
("keep OFF: needs a validation corpus first")**. Each one changes retrieval / learning /
advisory behavior, and the codebase docs mark them Tier C/S "needs harness / corpus". This
file records, per flag, the concrete recipe to earn a default-on decision: **which harness,
the A/B method, the metric, the pass bar, and the data status**.

The method is always the same shape — an **A/B**: run the flag OFF (baseline) and ON against a
labeled corpus, and require ON to beat OFF on the target metric *without regressing a control
set*. A flag is only a default-on candidate once its A/B is green and repeatable.

Data status legend: **✅ built** (corpus in this repo) · **🟡 partial** (some fixtures /
harness exist) · **⬜ needed** (must be authored).

---

## memory_negation_enabled  — ✅ built

- **What it gates:** extracts `not_<token>` negation tokens from memory content + queries
  (`memory_core_helpers_b.c`, `memory_core_search_c.c`) so negative facts ("X is not Y") are
  retrievable.
- **Corpus:** [`tests/eval/memory_negation_corpus.json`](../../tests/eval/memory_negation_corpus.json)
  — 19 fixtures / 20 cases. Every negated fact is paired with a positive distractor stating the
  opposite (`n001` "redis WITHOUT persistence" ⇔ `n101` "redis persists via AOF"), plus positive
  controls. This is what makes the metric discriminating: a negation-blind retriever scores both
  members of a pair highly on a negation query.
- **A/B:** load the corpus through the memory-retrieval eval (`mem_eval_load_corpus`,
  `server/agent_eval_memory_support.c`) with `memory_negation_enabled` false then true.
- **Metric:** recall@5 on the `neg*` + `dis*` cases (negation + discrimination). **Control:**
  recall@5 on the `pos*` cases must not drop.
- **Pass bar:** ON raises negation/discrimination recall by ≥ 10 points absolute with **zero**
  regression on the positive controls.
- **Why separate from the golden:** the gated `memory_retrieval_corpus.json` has a baked
  `n_cases`/baseline with a 5% regression gate; adding hard negation cases there would break it.
  This corpus is standalone and run explicitly for the A/B.

## memory_abstain_enabled  — ⬜ needed (harness extension)

- **What it gates:** a confidence/abstention gate that returns *nothing* when no memory is
  relevant, instead of surfacing a weak top-k.
- **Corpus needed:** answerable queries (expected = a real fid) **and** unanswerable queries
  (expected = `[]`, nothing in the corpus should match). The existing ndcg/recall harness has no
  abstention metric — it needs a small extension.
- **A/B / metric:** OFF vs ON; **abstention precision** = of the unanswerable queries, fraction
  correctly returning empty; **answerable recall** must not drop (no over-abstention).
- **Pass bar:** abstention precision ≥ 0.9 on unanswerable queries with < 2% answerable-recall
  loss. Reuse the negation corpus's fixtures as the "haystack" and add ~15 out-of-corpus queries.

## memory_scenes_enabled  — ⬜ needed (session-structured corpus)

- **What it gates:** clusters conversation turns into scenes and boosts within-scene retrieval.
- **Corpus needed:** multi-turn **session** transcripts with labeled scene boundaries (the flat
  `{fixtures,cases}` shape can't express this — needs a session/turn corpus). ~6–10 sessions,
  each 15–40 turns spanning 2–4 topic scenes, with per-query the expected in-scene turns.
- **A/B / metric:** scene boost OFF vs ON; **precision@5 of in-scene results** for a query issued
  mid-scene; **control:** cross-scene recall must not collapse.
- **Pass bar:** in-scene precision up ≥ 8 points with no cross-scene recall regression.

## memory_fetch_budget_enabled  — 🟡 partial (mem_eval + cost)

- **What it gates:** dynamic sizing of how many candidates to fetch before rerank.
- **Corpus:** reuse the golden `memory_retrieval_corpus.json` (no new labels needed) but capture
  **cost** (candidates fetched / tokens) alongside recall.
- **A/B / metric:** budget OFF (fixed k) vs ON; plot **recall@10 vs mean candidates fetched**.
- **Pass bar:** ON holds recall within 1% while cutting mean fetch by ≥ 20% (a cost win), OR
  raises recall at equal fetch. This is a cost/quality A/B, not a pass/fail correctness gate.

## code_trust_actuation_enabled  — 🟡 partial (code-vector-graph bench)

- **What it gates:** applies earned per-file "trust" as an RRF tie-break in code hybrid fusion.
- **Corpus:** extend `benchmarks/code-vector-graph` with code-search queries whose relevant files
  have accumulated trust signals (prior successful edits) vs cold files.
- **A/B / metric:** OFF vs ON; **MRR / ndcg@5** on code-search queries where trust should break
  ties toward the proven file. **Control:** queries with no trust signal must be unchanged.
- **Pass bar:** ndcg@5 up on the trust-relevant subset, flat on the control subset.

## learning_implicit_repeat_question / _repeated_correction / _workflow_repetition / _retrieval_outcome  — 🟡 partial

- **What they gate:** implicit-learning detectors that emit proposals from live turn patterns.
  These are **stateful and not replayable** without a live router, per `config.h` / the Tier-C
  notes — the hardest to corpus.
- **Existing harness:** `tools/learning_eval.py`, `src/tests/learning_implicit_replay.c`, and
  `src/tests/test_retrieval_outcome_bridge.c` already replay some detector fixtures.
- **Corpus needed:** labeled multi-turn traces per detector — e.g. for `_repeat_question`, a
  session where the user re-asks a question and the detector should fire exactly once; negative
  traces (paraphrase that is *not* a repeat) as controls.
- **A/B / metric:** replay traces with the detector OFF vs ON; **precision** (fired only on true
  positives) and **recall** (fired on all true positives). Because these feed the live proposal
  router, the bar is precision-first.
- **Pass bar:** ≥ 0.9 precision on the labeled traces with no false-fire on the negative controls,
  per detector, before any is considered for default-on.

## guardrails_blast_radius_advisory_enabled  — ✅ built (deterministic)

- **What it gates:** surfaces the code-graph structural blast radius (dependent files) before an
  edit; advisory and fail-open.
- **Corpus:** [`blast_radius_corpus.json`](blast_radius_corpus.json) — 4 fixtures / 6 cases, each a
  small code graph (files with exports + imports) with the ground-truth `(edited file → expected
  dependents + dependencies)`. Faithful to `db2_code_index_blast_radius` (src/db2/code_index.c):
  covers the basic importer set, the `has_exports` gate (no exports ⇒ no dependents), the hub
  threshold, and direct-vs-transitive (only direct importers advised).
- **A/B / metric:** it either lists the true dependent set or not; **precision/recall of the
  advised dependent files** vs the graph ground truth. No LLM in the loop, so this is a clean
  deterministic gate.
- **Pass bar:** recall = 1.0 (never miss a real dependent) with precision ≥ 0.8 (few spurious
  files) on the fixture set.

## delegate_graph_context_enabled  — ✅ built (deterministic)

- **What it gates:** prepends code-graph structural context to a delegate prompt; fail-open.
- **Corpus:** [`delegate_graph_corpus.json`](delegate_graph_corpus.json) — 2 fixtures / 4 cases.
  `delegate_inject_graph_context` builds its block from the same `kb_client_index_blast_radius`
  computation, so this shares the blast-radius graph: each case is a delegate prompt referencing a
  file with the neighbours the injected block must contain, plus fail-open cases (isolated file /
  no referenced path ⇒ no block) and a must-not-contain (transitive file) control.
- **A/B / metric:** assert the injected context contains the true neighbours (callers/callees,
  defining file) and stays within the budget. Since it's fail-open and advisory, the gate is
  "context is correct + bounded", not a downstream quality metric.
- **Pass bar:** injected context includes the ground-truth structural neighbours for every
  fixture and never exceeds the configured budget.

---

## Running the memory_negation A/B (worked example)

The negation corpus is the one that's fully built. Load it through the same eval entrypoint the
golden corpus uses (`aimee eval memory-retrieval` / `mem_eval_load_corpus`), once with
`memory_negation_enabled: false` and once `true`, and compare recall on the `neg*`/`dis*` cases
against the `pos*` controls. Green + repeatable ⇒ `memory_negation_enabled` graduates from
Bucket 3 to a default-on candidate.

The remaining flags graduate the same way as their corpora are built out (status column above).

## Running the deterministic harness (blast_radius + delegate_graph)

The two deterministic corpora have a runnable harness: **`aimee-blast-radius-eval`**
(`src/tests/blast_radius_eval_main.c`, `make ../aimee-blast-radius-eval`). It loads each
fixture into an ISOLATED throwaway schema in a disposable Postgres (the eval temp-store, gated
on `AIMEE_DB2_EVAL_URL` — never the production DSN), runs the REAL `db2_code_index_blast_radius`,
and scores precision/recall against the corpus ground truth. delegate_graph reuses the same
computation (its `expected_neighbours` = dependents + dependencies of the referenced file).

```
AIMEE_DB2_EVAL_URL=postgres://<owner>@<host>/<db> \
  ./aimee-blast-radius-eval benchmarks/bucket3-defaults/blast_radius_corpus.json \
                            benchmarks/bucket3-defaults/delegate_graph_corpus.json
```

**Result (real pgvector integration Postgres):** blast_radius 6/6 cases —
dependents recall=1.000 precision=1.000, dependencies 3/3; delegate_graph 4/4;
**PASS**. Both `guardrails_blast_radius_advisory_enabled` and `delegate_graph_context_enabled`
now have a green, repeatable A/B ⇒ default-on candidates.
