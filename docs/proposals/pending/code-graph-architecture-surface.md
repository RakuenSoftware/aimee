# Proposal: the code graph should carry the architecture, not just the symbols

- **State:** pending — five slices, ordered. Slice 1 is the only real extractor
  work; 2–5 are analysis and presentation layers over the graph we already keep
  in DB2.
- **Author:** JBailes
- **Date:** 2026-07-11
- **Charter roles:** Extract (route/storage extraction), Detect-Cluster (health
  and hotspot findings over the enriched graph), Recall (orientation injected at
  session-start), Calibrate (per-finding confidence, coverage as an honesty
  envelope), Constrain-Verify (baseline/diff as a review-gate signal).

## Thesis

The code graph indexes symbols, references, and call edges — `src/index.c`,
`src/extractors_extra.c`, keyed on the shared ontology in
`src/headers/memory_ontology.h` (`NODE_FILE`, `NODE_FUNCTION`, `NODE_STRUCT`,
`NODE_MODULE`, …). That graph answers "who calls this" and "what does an edit
touch," and it does it from a database lookup instead of a filesystem scan. Good.
But it stops at the call graph. It does not know the shape of the thing it is
indexing: where the API surface is, where the data lives, or what the repo looks
like from ten thousand feet. The two questions that actually decide a review —
*which endpoints does this migration touch* and *did this change just add a
cycle* — the graph cannot answer today.

The gap is that the graph carries facts about functions but nothing about the
**architecture** those functions add up to. A route is a symbol we don't label a
route. A table write is a call we don't label a write. A repo's shape is a set of
counts and outliers we already have the edges to compute and simply don't. Five
slices close that gap, and every slice after the first is a projection over edges
we're already storing.

None of this is neural. Every finding here is a deterministic graph fact or a
labelled heuristic — same commit, same output. That is the point: the structure
is the part the AI should never be guessing at, and it's the part we can hand it
for free.

## Slice 1 — routes and storage as first-class node kinds

Extend the ontology with `NODE_ROUTE` (an HTTP/RPC endpoint) and `NODE_STORAGE`
(a table, collection, or store), and two edges: `handles` (route → the symbol that
serves it) and `touches` (symbol → storage, read or write). The extractors already
walk the tree-sitter parse; the work is teaching them to recognise the framework
idioms that mint these nodes — a decorator/annotation/router-registration for a
route, an ORM model or query call for a storage node. Start with the frameworks we
actually work in and let the rest fall through to nothing (a missing route is a
coverage gap, slice 4, not a wrong answer).

This is the one slice with real cost, and it's the one worth paying for. Once a
route is a node, blast-radius crosses from "these files change" to "these
endpoints change." Once a table is a node, a schema edit shows its consumers the
same way a shared-library edit already does. The seed ontology in `src/rel_types.c`
is self-validating (a bad row fails the build), so the new kinds and edges get
checked, not trusted.

## Slice 2 — an orientation pass, injected at session-start

`aimee graph explain` (`src/cmd_graph.c`) is node-local: give it a file or symbol,
get the edges incident to it. There is no repo-level view — nothing that says what
the codebase *is*. Add one: an orientation pass that returns the shape from the
graph we already hold — kind counts, the API and data surface (slice 1), import
cycles and depth outliers we already find in `src/code_audit_graph.c`, and the
modules ranked by fan-in plus fan-out with a criticality tier.

The value isn't the CLI, it's the injection. A compact form of this belongs in the
session-start context (`cmd_hooks.c` / `cmd_session_lifecycle.c`) so the agent opens
already oriented instead of re-deriving the layout on every task — the exact
rediscovery the graph exists to kill. It's a Recall surface: the map, handed over
before the first tool call, not reconstructed after twenty.

## Slice 3 — a baseline and an architectural diff, as a review gate

`aimee graph baseline` pins the current graph state; `aimee graph diff` reports the
delta against it: cycles added or broken, a new god-class, coupling that moved,
routes and storage and symbols added or removed. This is a Constrain-Verify signal,
and it wires straight into the workflow review gates (`src/workflow/wfe_*.c`) and
the roundtable: "did this PR add a cycle or a new hotspot" is a structural fact a
reviewer can act on, and one no delegate is currently able to compute. Pin on
entry to a change, diff on the way out.

## Slice 4 — coverage and provenance: the graph says where it's blind

A graph that quietly stops at the edge of what it parsed will hand the agent a
confident, wrong blast-radius. Make it honest instead. Each scan stamps its
provenance — git ref, scan id, content hashes, extractor versions — and reports its
coverage: unresolved cross-repo edges, files it couldn't parse, framework idioms it
didn't recognise. Blast-radius and the orientation pass then carry an envelope: *of
the callers I can see, here they are; here is the fraction I can't.*

This is the Calibrate half of the graph and it fits the platform's confidence and
abstention discipline exactly. An answer that names its own blind spots is worth
more than one that pretends it has none, and it's the difference between a tool the
agent can trust to abstain and one it learns to second-guess.

## Slice 5 — impact grouped by distance, findings scored by confidence

Two small changes to how results come back. Blast-radius returns a flat file list
today; group the dependents by hop distance with a criticality tier, so "one call
away and on a hot path" reads differently from "six hops out through a test." And
every audit finding carries a confidence: a structural fact — a cycle exists, an
export is unused — is 1.0; a heuristic — a god-class flagged as a statistical
outlier — sits below it and says so. The agent triages instead of treating a
measured cycle and a fan-in guess as the same claim.

## Ordering and cost

Slice 1 pays for the other four — routes and storage are the nodes 2, 3, and 5 get
their leverage from, and 4 is most useful once there's a framework surface to be
blind to. But 2, 3, and 5 stand alone on today's call graph and can ship before 1
lands: an orientation pass, a review diff, and hop-grouped impact are all just
projections over edges already in DB2. Suggested order: 2 and 5 first (cheap,
immediately useful, no extractor work), then 1 (the real build), then 3 and 4 on
top of the enriched graph.

## Honest limit

Framework recognition is a long tail and slice 1 will never close it — every stack
has its own way of registering a route, and we'll cover the ones we live in and
miss the rest. That's why slice 4 exists: the graph is allowed to not know a route,
it is not allowed to pretend it saw everything. Coverage is the release valve on
the extractor's incompleteness, and shipping 4 alongside 1 is what keeps a partial
extractor honest rather than misleading.
