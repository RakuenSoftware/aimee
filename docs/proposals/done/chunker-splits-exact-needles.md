# The page chunker splits exact needles, defeating a documented guarantee

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Delivered scope archived 2026-07-26.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

*Filed from measurement taken while implementing the retrieval proposal's
Slice 5. Classification: **correctness, medium**.*

> **RESOLVED by removing chunking.** Both failures below were consequences of
> pre-cutting the page into fixed-size boxes. Extraction now centres a window on
> each match, so a needle cannot be split by a boundary (Failure 1 is
> unrepresentable, not merely rare) and there is no per-chunk literal count to
> exhaust (Failure 2 does not arise). The tool description has been reworded to
> what is deliverable. Retained as the record of how the defect was found and
> why the boundary-backoff patch that was written for it was dropped in favour
> of deleting the chunker.

## The claim that is not true

`web_read`'s tool description promises callers:

> "returns only the query-relevant spans (exact API/error/version needles
> guaranteed in)"

That guarantee fails in two distinct ways, neither of which is a ranking
problem. Both were found by measurement, not inspection.

## Failure 1: the chunker splits the token

`chunk_text` cuts at a target size (~480 bytes), backing off to a nearby newline
or period. It has no notion of token boundaries. When the cut lands inside the
needle, the token exists in the page but in **no chunk**, so no amount of
ranking can retrieve it.

Measured over 500 generated pages carrying an exact identifier at a random
depth: **1.6% lost the needle to a chunk boundary**. Every selector is affected
equally, because the loss happens before ranking runs.

## Failure 2: more literal hits than the budget can hold

`WEBREAD_BUDGET` is 1500 bytes, roughly three spans. A page with 60+ literal
hits cannot emit them all under any ordering. A deeply-placed target is dropped
whether the emission order is literal-first or fusion-ranked.

This one is not a defect (it is the budget doing its job) but the
promise as worded does not admit it.

## What was tried and rejected

A literal-first emission tier was implemented to "restore the guarantee", then
removed, because measurement showed it restored nothing:

- Across **49,220** pages where the needle survived chunking, weighted fusion
  without any tier dropped it **0 times**. The tier changed no outcome.
- On pages where the needle *is* lost, the tier does not help, because the loss
  is caused by chunking or by budget exhaustion, not by ordering.

The tier had been justified by attributing the 1.6% loss rate to ranking. It was
the chunker. Recording this so the same mistake is not repeated: an intervention
justified by a misattributed measurement is worse than no intervention, because
it looks like a fix.

## Direction

Two independent options, either useful alone:

1. **Boundary-aware chunking.** When the cut point falls inside a run of
   identifier characters, back off to the preceding boundary. Cheap, local to
   `chunk_text`, and closes Failure 1 outright. Needs care so backing off cannot
   produce an empty or pathologically short chunk.
2. **Overlapping chunks.** A small overlap between adjacent spans makes a split
   token appear whole in at least one chunk. Costs budget and introduces
   duplicate content across spans, so it interacts with selection.

Option 1 is the smaller change and addresses the measured failure directly.

Separately, the tool description should be reworded to what is deliverable:
exact matches are **prioritised** and retrieved when they survive chunking and
fit the budget. A promise a caller can rely on is worth more than a stronger one
that fails 1 time in 60.

## Acceptance

- A needle placed at any offset in a page is present in at least one chunk.
- The generated-corpus measurement above reports 0 chunk-split losses.
- No chunk becomes empty or degenerate as a result of boundary backoff.
- The tool description states the achievable guarantee.
