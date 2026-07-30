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

## This IS a correctness gap, and it is measured

> **Correction (2026-07-29).** An earlier version of this proposal claimed the
> selection benchmark scored all candidates prefix-free, and concluded that
> prefix-free serving therefore reproduced the measurement. **That was wrong.**
> The sweep runner gave **each model its own card-recommended prefix**
> (`nomic → search_query:/search_document:`, `e5 → query:/passage:`,
> `magibu → task: search result | query: …`, and `""` for models whose cards
> define none, such as bekko-a25m). The conclusion below is reversed accordingly.

The benchmark measured every model **with** its prefixes. aimee serves **without**
them. So for any prefix-dependent model, the deployed system does **not** reproduce
its benchmark score — the selection number is not the number production gets.

This was measured directly on the frozen-ab-v1 suite, same harness, same corpus,
varying only the prefix:

| model | with card prefix | prefix-free (what aimee serves) | delta |
|---|---:|---:|---:|
| nomic-embed-text-v2-moe | 0.6072 | **0.5823** | **−0.0249** |
| bekko-a25m (card defines none) | 0.5909 | 0.5909 | ±0 (nothing to strip) |

The prefixed column carries the **re-measured** values from
[embedder-selection-frozen-ab-v1](../../validation/embedder-selection-frozen-ab-v1.md);
the original sweep recorded 0.6058 and 0.5892, and the reproduction check landed
at 0.6072 and 0.5909 — within the suite's GPU noise. An earlier revision of this
proposal quoted the original pair against the re-measured prefix-free figure,
which mixed runs and understated the delta as −0.0235.

**The consequence is decision-changing.** nomic's margin over a25m exists only
*with* prefixes. Strip them — which is what the current code does — and the
ordering inverts: 0.5823 against 0.5909. A model that needs no prefix carries its
benchmark score into production intact; a prefix-dependent one does not.

So this is not "upside we are leaving on the table". It is a **silent regression
between what was measured and what is served**, and it is large enough to select
the wrong model.

## Why it stayed invisible

Nothing errors. The vectors are well-formed, correctly dimensioned, correctly
pooled and correctly normalised — they are simply the vectors for a subtly
different input than the one that was benchmarked. The same class of failure as
the `AIMEE_LLM_EMBED_POOLING` default: silently wrong, not loudly broken.

## The decision this forces

There are two coherent positions, and the embedder choice is downstream of which
one is taken:

1. **Implement prefix support**, then a prefix-dependent model can be deployed at
   the score it was selected on.
2. **Do not implement it**, and then only prefix-free models may be selected —
   the selection must be re-ranked on prefix-free numbers, because those are the
   ones production will deliver.

What is *not* coherent is selecting on prefixed scores and serving prefix-free.
That is the current state, and it is how a 0.6072-vs-0.5909 win became a
0.5823-vs-0.5909 loss without anything appearing to go wrong.

## Migration cost, which is the real constraint

Prefixes change every vector. Adopting them is not a config toggle — it is a
**full re-embed** of the entire corpus, through the same double-gated dim-change
machinery used for a dimension change (`aimee kb reembed` /
`db2_dim_change_reset`), even though the dimension itself is unchanged.

Note the asymmetry this creates in model selection: a model whose card defines no
prefix (bekko-a25m) has no gap between benchmark and production and needs none of
this machinery. That is a real, if unglamorous, architectural advantage, and it
should be weighed alongside raw NDCG rather than treated as a tie-breaker.

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
