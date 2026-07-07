# Proposal: the KB fusion "lexical" leg is a duplicate dense search — give it a real FTS leg

- **State:** proposed (pending — not started)

## Finding (code + live verified)

`kb_search_fused` (`src/kb/kb.c`) blends two legs — a "lexical" leg
(`lexical_search_via_vector`) and a dense leg (`vec_search`) — and offers three
fusion modes (`rrf` / `static_alpha` / `dynamic_alpha`). **But both legs call the
same function with the same query embedding:**

```c
// lexical_search_via_vector (kb.c:1140)
int qdim = memory_embed_text(query, effective_cmd, qvec, EMBED_MAX_DIM);
n_hits = pgvec_kb_vector_search_project(project, qvec, qdim, cap, ids, scores, ...);

// vec_search (kb.c:1176)
n_hits = pgvec_kb_vector_search_project(project, qvec, qdim, max * 2, ids, scores, ...);
```

They differ only in which field they store the score in (`lex_score` vs
`dense_score`). The FTS index that exists on the corpus (`kb_documents.kb_fts_tsv`)
is **never queried** by the fusion path — grep for `to_tsquery` / `ts_rank` /
`bm25` / `kb_fts_tsv` in `kb.c` returns nothing.

### Consequence: every fusion mode is provably equivalent

`alpha_merge` computes `final(d) = (1-α)·dense_norm(d) + α·lex_norm(d)`. Because
the two legs return the same docs with the same scores, `lex_norm(d) ==
dense_norm(d)`, so `final(d) = (1-α)·x + α·x = x` for **any** α. `rrf_merge` over
two identical ranked lists is likewise the identity. So `rrf`, `static_alpha`, and
`dynamic_alpha` cannot differ.

### Evidence

Live on `.254` (fully-embedded corpus, `kb_ranker_enabled=0`): across 10
corpus-aligned fixtures + 22 diverse probe queries (identifier-style *and* prose),
**all three modes returned identical top-5 orderings for every query** — 0/32
differentiated. The benchmark harness (`run.py`) and probe (`probe_diff.py`) that
produced this are in `benchmarks/kb/dynamic-alpha/`.

This means the `user-selectable-fusion-mode` feature and the whole dynamic-alpha
question are moot until the lexical leg is a genuinely different retrieval signal.

## Fix

Make the lexical leg a real term/FTS retrieval so it *diverges* from the dense
leg, giving α something to arbitrate:

- Replace `lexical_search_via_vector`'s body with a Postgres FTS query over
  `kb_documents.kb_fts_tsv` (`websearch_to_tsquery` + `ts_rank_cd`), scoped by
  `project`, returning `(doc_id, ts_rank)` into `lex_score`. Keep the same
  `kb_result_t` shape so `alpha_merge` / `rrf_merge` are unchanged.
- The dense leg (`vec_search`) stays as-is.
- Now identifier/exact-token queries surface docs the dense leg misses (BM25-style
  exact match), and `dynamic_alpha`'s high-α lexical boost has real effect;
  prose queries lean dense. The fusion modes become genuinely distinct.

## Verification (acceptance)

- Re-run `run.py` + `probe_diff.py` against a KB with the FTS leg: the modes must
  now differ on a non-trivial fraction of queries (target: dynamic_alpha wins the
  `positive`/identifier fixtures without regressing the `false_positive_guard`
  prose ones — the per-shape split the harness already reports).
- Unit: a fixture where an exact rare token exists in doc A but doc A is not the
  dense-nearest — assert the lexical leg ranks A and dynamic_alpha promotes it.

## Non-goals

Not changing `alpha_merge` / `rrf_merge` / `kb_fusion_predict_alpha` — those are
correct; they were just being fed two copies of the same signal. Not touching the
facet/artifact search (`kb_http_search_facets`), which is a separate path.
