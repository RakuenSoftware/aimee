# Preview fixture isolation — 2026-09-23

The fresh `9868d3a69` candidate passed T3's 614 checks. T2 stopped at the
summary-preview negative fixture: the background indexer replaced its synthetic
headline with canonical content and recorded a new valid producer observation.
The history checks passed before that failure. This is not a passing full T2 run.

The fixture now holds the existing relation-consumer advisory barrier while
atomically marking only its own indexing jobs done on creation and parent edits.
The barrier prevents journal replay from re-enqueuing those jobs; consuming the
jobs in the writing transaction also excludes an already-running index batch.
The application worker, serving predicates and all assertions remain unchanged.

Consuming fixture jobs alone was insufficient: the retained
[first replay](memory-preview-fixture-2026-09-23/fixed.txt) reproduced the journal
re-enqueue race. With both controls, five consecutive repetitions passed all
80 preview assertions after eight history checks, for
[88 passing checks](memory-preview-fixture-2026-09-23/checks.json).
The [replay output](memory-preview-fixture-2026-09-23/barrier.txt) records the
individual assertions. Disposable replay stacks were removed afterward.

This validates fixture isolation against the existing candidate image. A fresh
full matrix remains required; no proposal completion is claimed.
