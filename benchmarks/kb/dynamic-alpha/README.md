# Dynamic-alpha fusion benchmark

Measures whether the `dynamic_alpha` fusion mode improves retrieval over the
`rrf` baseline (and `static_alpha`) on a labelled query set — reporting the
**per-shape better/worse split**, not a single average, because dynamic alpha is
expected to be a *mixed* result: win on lexical/identifier queries without
regressing semantic ones.

## ⚠️ Key finding (2026-07-07): the fusion is currently degenerate

Running this harness against the live `.254` KB (fully embedded, `kb_ranker_enabled=0`)
produced **identical top-5 orderings across `rrf`, `static_alpha`, and `dynamic_alpha`
for all 32 queries** (10 corpus-aligned + 22 probe). Root cause, verified in code:
`kb_search_fused`'s "lexical" leg (`lexical_search_via_vector`) is a **duplicate of
the dense leg** — both call `pgvec_kb_vector_search_project` with the same query
embedding; the FTS index (`kb_fts_tsv`) is never queried. So `alpha_merge` collapses
to identity (`(1-α)·x + α·x = x`) and no fusion mode can differ. See
`docs/proposals/pending/fusion-lexical-leg-is-duplicate-dense.md`. **Until the
lexical leg is a real FTS/BM25 signal, treat mode A/B as N/A, not "no benefit."**

- `corpus-aligned-fixtures.json` — self-labeled queries whose targets exist + are
  embedded on the aimee corpus (`--fixtures` this for a run that actually resolves).
- `probe_diff.py` — runs candidate queries across modes and flags where the top
  result differs (the tool that surfaced the 0/32 result).

## Run

```sh
# against a live aimee-kb (POST /v1/search); no third-party deps
python3 run.py --endpoint http://127.0.0.1:8741 --project aimee --k 5 --out report.json

# remote KB over the LAN
python3 run.py --endpoint http://192.168.1.254:8741 --project aimee
# add --bearer <token> if the endpoint requires one
```

Exit code is non-zero if `dynamic_alpha` regresses a `false_positive_guard` /
`regression` fixture versus `rrf` — so it can gate CI once a stable corpus exists.

## Files

- `fixtures.json` — the charter query set (3 `positive`, 2 `false_positive_guard`,
  2 `regression`), each with an `expected_top_path_substring`.
- `run.py` — the A/B runner (this harness).
- `before_report.json` / `after_report.json` — the 2026-05-03 spike run.

## Known limitations before this yields a verdict

The 2026-05-03 closeout ("gate not met, default stays `rrf`") was made on **empty
data** (Qdrant D-state, no corpus). Re-running today surfaced two blockers to fix
before the harness produces a real go/no-go:

1. **Corpus health.** `POST /v1/search` is the fusion path (`kb_search_fused`), and
   it applies the mode correctly (`fusion_mode_used` reflects the request). But on
   a KB whose doc-embed/curator drain has been wedged, doc embeddings are
   incomplete and retrieval is undiscriminative (flat ~0.03 scores, one artifact
   dominating) — every fixture misses in every mode. Run only against a KB whose
   curator has fully drained (see the curator-drain lease fix).
2. **Doc-only surface vs. code fixtures.** `/v1/search` ranks `doc_chunk`s from
   `kb_documents`; three fixtures (`lexical_*`) expect **code** files
   (`config_learning.c`, `kb.c`) that live in `code_embeddings`, not the doc
   corpus, so they can never match here. Either point those fixtures at
   proposal/doc targets that exist in the doc corpus, or add a code-search variant
   of the harness against `/v1/code/*`.

Until (1) is cleared on the target KB, treat an all-miss result as
*inconclusive*, not as evidence against dynamic alpha.
