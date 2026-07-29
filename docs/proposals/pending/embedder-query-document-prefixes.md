# Proposal: query/document prefixes for the embedder

- **State:** 🟡 **PENDING — problem recorded, not yet designed.** Written down at
  the nomic cutover (2026-07-29) so the gap is tracked rather than rediscovered.
  No implementation is proposed here yet; the measurement that would justify one
  is defined below.
- **Owner:** unassigned.

## Problem

**aimee has no query/document prefix plumbing anywhere.** There is no config
field, no embed-path parameter, and no call site that distinguishes "this text is
a query" from "this text is a document" before handing it to the embedder.

Modern retrieval embedders are trained asymmetrically and expect a prefix on each
side:

| model | query prefix | document prefix |
|---|---|---|
| nomic-embed-text-v2-moe (current default) | `search_query: ` | `search_document: ` |
| Qwen3-Embedding (previous default) | `Instruct: …\nQuery: ` | (none) |
| bge-* | `Represent this sentence…` | (none) |
| multilingual-e5-* | `query: ` | `passage: ` |

**This predates nomic and is not a regression.** The previous Qwen3 embedder was
also served without its instruction prefix. The cutover neither introduced nor
worsened the gap; it is recorded now because the boundary was examined during
that work.

## Why this is NOT a correctness bug

The selection benchmark
([embedder-selection-frozen-ab-v1](../../validation/embedder-selection-frozen-ab-v1.md))
scored **all 14 candidates prefix-free** — the harness's `--query-prefix` and
`--doc-prefix` both defaulted to empty, and the result artifacts record no
prefix. So prefix-free serving is exactly what reproduces the measured 0.6058
NDCG@10. Production matches the benchmark.

Adding prefixes is therefore **plausible unmeasured upside, not a fix.** Nothing
is currently broken relative to what was measured.

## Why it still matters

Uniform treatment is not neutral treatment. Models differ in how strongly they
depend on their prefixes, so a prefix-free sweep understates prefix-dependent
models by an *unequal* amount. The `aimee-encoder` selection document already
records this caveat for bge-small and e5.

Two consequences:

1. **The current default may be leaving quality on the table.** nomic is
   prefix-dependent and won anyway, so its measured score is a floor.
2. **The ranking itself carries a caveat.** A prefix-aware re-run could reorder
   the candidates. This does not invalidate the decision — it bounds how much
   confidence the ordering deserves.

## What must be measured before proposing an implementation

1. Re-run `eval/frozen-ab-v1` for the top candidates **with** each model's
   card-specified prefixes, against the existing prefix-free numbers. Same suite,
   same manifest SHA, so the runs are directly comparable.
2. Report per-`doc_kind` deltas, not just the aggregate — the prose and code
   buckets may respond differently, and `cited_artifacts` is where the runner-up
   already wins.
3. Confirm whether the ranking of the top candidates changes at all.

If the lift is not material, the correct outcome is to **record the measurement
and change nothing**.

## Migration cost, which is the real constraint

Prefixes change every vector. Adopting them is not a config toggle — it is a
**full re-embed** of the entire corpus, through the same double-gated dim-change
machinery used for a dimension change (`aimee kb reembed` /
`db2_dim_change_reset`), even though the dimension itself is unchanged at 768.

That asymmetry is the crux: the change is cheap to implement and expensive to
deploy. It should not be undertaken on the expectation of a gain — only on a
measured one.

## Non-goals

- Not a per-call-site "is this a query?" refactor of the retrieval stack. Scope
  the seam at the embed boundary.
- Not a change to the selected model. This is orthogonal to which embedder runs.

## Open questions

- Where does the query/document distinction actually live today? The embed path
  is shared by memory search, code embedding (`intent_vec` / `sig_vec` /
  `body_vec`) and curator artifacts; some of those are unambiguously documents,
  but the seam has not been traced.
- Do the three code vector kinds want the document prefix, or something else?
  `body_vec` holds raw code, which is not natural-language prose.
- How should a prefix be tied to the model identity so a future embedder swap
  cannot silently inherit the wrong pair — the same failure mode as the
  `AIMEE_LLM_EMBED_POOLING` default, which was `last` and silently wrong for
  nomic until the cutover caught it?
