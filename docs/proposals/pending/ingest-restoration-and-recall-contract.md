# Proposal: ingest restoration (damage-as-catalyst) + bounded-hallucination recall contract

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter role(s):** knowledge-base ingest + recall (kb-side intelligence
  pass; reuses the existing curator synthesis path and graph provenance edges —
  no new store, no new DB tier).
- **Scope:** `src/kb/kb_lab.c` + `src/headers/kb_lab.h` (turn the terminal
  `REJECT`/`REVIEW_NEEDED` stages into a restoration entry point), `src/kb/kb_curator_synthesize.c`
  (restoration-biased fusion of a fragment against its nearest complete
  neighbour, with provenance), `src/kb/kb_service_graph.c` (provenance edges —
  already carry `source`), `src/memory_graph_fusion.c` + `src/kb/kb.c` (recall
  contract: tag each result verbatim vs synthesised), unit tests, docs. No new
  long-lived service.

## Summary

Two ideas, treated here as general concepts (independent of any particular
typed-particle or "self-repairing index" framing they sometimes carry):

1. **Damage-as-catalyst ingest.** aimee's ingest lab already *detects* damaged
   documents and then **drops them**. Route them instead into a restoration
   queue that fuses each fragment against its nearest complete neighbour to mint
   a repaired, provenance-tracked document.
2. **Bounded-hallucination recall contract.** aimee has **no extractive
   guarantee** today. Tag each recall result as **verbatim** (stored text, zero
   synthesis) or **synthesised** (with a confidence and citations), and mark
   genuine gaps `[unknown]` rather than fabricate.

```
  ingest ──▶ kb_lab quality audit ──▶ READY ───────────────────▶ index
                     │                 REVIEW_NEEDED / REJECT
                     │                        │   (today: dropped/flagged)
                     ▼                        ▼   (proposed: restoration)
            heuristic completeness    fuse fragment × nearest complete
            signal (free, no LLM)     neighbour under restoration guardrail
                                      → mint repaired doc + provenance edge
                                                │
  recall ◀── verbatim | synthesised(conf) ◀─────┘   (bounded-hallucination tag)
```

## Motivation

What aimee already has (verified):

- `kb_lab.c` **detects** damage — `FLAT_TEXT`, `OVERSIZED_CHUNK`,
  `UNDERSIZED_CHUNKS`, `BINARY_NOISE`, `EMPTY_FILE`, `TABLE_SPLIT` — and sorts
  docs into `READY` / `REVIEW_NEEDED` / `REJECT` (`kb_lab.c:246,575,658`). The
  damaged stages are a **dead end**.
- The deep curator already does **LLM-mediated synthesis with provenance +
  citations** (`kb_curator_synthesize.c:198`, `kb_curator_promote.c:186`), over
  typed artifacts (`entity` / `summary` / `relation` / `fact` / `claim` /
  `narrative`).
- Graph edges already carry `source` (`kb_service_graph.c:98`); fusion recall
  merges lexical + vector hits (`kb.c:1348+`, `memory_graph_fusion.c`).

What is missing — and what damaged-record retrieval techniques highlight:

- The detector and the synthesiser are **joined nowhere**: a damaged doc is
  rejected rather than repaired. (Note: `memory repair` (`kb_service.c:183`) is
  *index* repair — re-embedding failed vectors — **not** content restoration.)
- There is **no verbatim/extractive contract** at recall time — "hallucination"
  appears in the codebase only as a dogfood *outcome label* (`cmd_dogfood.c`),
  never as a per-result guarantee.

## Design

### Part 1 — restoration path (turn REJECT into a repair entrance)

1. **Heuristic completeness signal, not per-chunk LLM.** Some techniques call the
   LLM to classify *every* chunk; aimee's `kb_lab` already produces completeness
   signals **for free**. Reuse them — do not pay per-chunk LLM cost. Completeness
   is an **orthogonal flag on existing artifacts**, not a new type system.
2. **Restoration queue.** `REVIEW_NEEDED` / `REJECT` docs (and undersized/flat
   fragments) enter a queue instead of terminating.
3. **Fusion.** For each fragment, find its nearest complete neighbour by cosine
   similarity (embeddings already exist) and fuse via the curator synthesis path
   under a **restoration-biased prompt**: preserve every noun, number, and
   relationship from the source; mark genuine gaps `[unknown]`; never invent.
   Mint a repaired document with a **provenance edge** (which fragment + which
   base produced it) — the graph already supports `source` edges.
4. **Bounded, gated, idempotent.** Single fusion pass by default (not multi-pass
   re-fusion, which compounds drift and cost); re-runs must be idempotent.

### Part 2 — bounded-hallucination recall contract

1. **Per-result provenance tag.** Every recall result is tagged **verbatim**
   (returned from stored text, no synthesis) or **synthesised** (fused from
   fragments, with confidence + citations). The curator's citation infra is
   already most of the way there.
2. **Extractive guarantee.** A high-confidence stored hit is returned verbatim —
   no LLM rewrite — so the caller can trust it as ground truth.
3. **`[unknown]` sentinel.** Adopt it in the curator synthesis/extraction prompts
   to suppress fabrication during fusion (a one-prompt change, low cost).
4. **Provenance-constrained traversal (refinement).** When answering from a
   synthesised node, bias graph hops along **provenance edges** (the docs that
   produced the node) rather than global k-NN over noisy rows. aimee already has
   fusion recall + provenance edges, so this is a testable refinement, not a
   rebuild.

### What this deliberately does **not** adopt

- Elaborate typed-particle "algebras" / physics branding — dressing. aimee's
  `entity/summary/relation/fact/claim/narrative` typing is richer; add
  completeness as a flag, not a new type system.
- **LLM-classify-every-chunk** — `kb_lab` gives completeness heuristically.
- Importing any prototype as a dependency — aimee already exceeds the retrieval
  sophistication (vector + graph + fusion + curator) of the demos these ideas
  come from.

## Safety (and the bridge to the optimisation surface)

Self-repair via LLM fusion is **expensive and drift-prone**, and prototype
implementations of it typically ship with **zero guardrails**. aimee has them:
gate every restoration pass behind the `memory.benchmark` suite + `baseline.json`
regression gate, track regressions via `memory_hard_negative_log`, and bound
synthesis confidence with calibration credible intervals. aimee is therefore
**better positioned to do this safely** than the demo-grade sources of the idea.

The two new thresholds this introduces —

- **repair-vs-reject** (when is a damaged doc worth restoring vs dropping?), and
- **verbatim-vs-synthesize** (confidence cutoff for the extractive guarantee) —

are exactly the kind of surface the companion proposal,
[optimization-surface.md](optimization-surface.md), tunes. Register both as
bandit **decision points** there. The two proposals compose: this one supplies
*new things to optimise*; that one supplies the *loop that tunes them safely*.

## MVP

1. Wire `kb_lab` `REVIEW_NEEDED` → restoration queue (start with
   `UNDERSIZED_CHUNKS` / `FLAT_TEXT` fragments only).
2. Single-pass curator fusion against nearest complete neighbour, with provenance
   edge + `[unknown]` sentinel.
3. Recall returns the verbatim/synthesised tag.
4. Gate behind the existing benchmark + hard-negative suite; default-off flag.

## Phasing

- **P1** — restoration queue + single-pass fusion (`UNDERSIZED`/`FLAT` only) +
  provenance edge + `[unknown]` sentinel; default-off.
- **P2** — recall contract: per-result verbatim/synthesised tag + extractive
  guarantee for high-confidence stored hits.
- **P3** — provenance-constrained traversal for synthesised answers.
- **P4** — expose repair-vs-reject and verbatim-vs-synthesize thresholds as
  decision points in [optimization-surface.md](optimization-surface.md).

## Risks

- **Fusion drift / fabrication** — bound to a single pass, `[unknown]` sentinel,
  and the benchmark/hard-negative gate; never auto-promote a repaired doc that
  regresses recall.
- **Cost** — restoration is LLM work; queue it, batch it, and keep it default-off
  until the offline suite shows net recall gain.
- **Provenance integrity** — every minted doc must carry an auditable edge back
  to its fragment + base; no orphaned synthesised nodes.
