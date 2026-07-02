# How aimee learns, and becomes your company's knowledge base

aimee is the institutional memory of an organization: one self-learning knowledge
base that distills what everyone knows across engineering, product, sales, support,
operations, legal, and finance, and makes it queryable by anyone in the terms of
whatever domain they work in.

It starts small and useful, as persistent memory for your AI coding tool, and the
same substrate scales up to a company-wide knowledge base. Nothing about the model
changes between "remember my database host" and "what are the engineering
implications of the new billing policy?". It is the same ingest, the same graph,
and the same retrieval at a different scope.

This document explains the mechanisms behind that. Most of the substrate ships
today: memory tiers, the curator extraction pipeline, the scope lattice, the
knowledge graph, idle reflection, and delegation. The full all-domain reach and the
deepest cross-domain synthesis are where the architecture is headed. Each capability
below summarizes the machinery that backs it.

---

## 1. One knowledge base, every domain

A shared `aimee-kb` is the organization's collective memory. The curator ingests
any document: prose, code, API specs, runbooks, design docs, tickets, meeting
notes, and policies. It extracts structured knowledge from each one. The pipeline
is domain-agnostic. It treats a document from an engineer and a document from a
sales lead the same way, pulling out entities, facts, decisions, and relationships
from both.

- **Backed by:** a doc-and-code extraction pipeline (the "deep curator") turns each
  ingested document into structured entities, facts, decisions, and relationships,
  running inside a single shared `aimee-kb` service that many users and projects
  connect to.

Knowledge is organized by a scope lattice, `global > workspace > project > user`,
so the same store holds company-wide truth, team context, and private notes without
them bleeding into each other.

- **Backed by:** a memory scope lattice with an audited public access contract
  governs what each scope (`global`/`workspace`/`project`/`user`) can read and
  write, so promotion outward is an explicit, recorded change.

---

## 2. Knowledge distills two ways

**Outward, the whole org compounds.** Everyone who works against a shared
`aimee-kb` inherits everyone else's distilled knowledge. A fix one engineer
learned, a constraint legal flagged, a customer pattern support noticed: once it is
in a shared scope, every future query by every person benefits from it. The team's
knowledge stops living in individual heads and chat logs and starts accumulating in
one place.

**Inward, aimee learns you.** The more you use aimee, the more it distills your
knowledge, preferences, decisions, and recurring patterns into durable memory and
an evolving personal profile. Your private (`user`-scoped) knowledge stays yours;
what you choose to share promotes outward through an audited scope-change.

- **Backed by:** a cross-source learning pipeline distills sessions and documents
  into durable memory and an evolving personal profile, splitting episodic
  experience from semantic fact and keeping a stable per-user identity that improves
  as you use it.

---

## 3. It learns, not just stores

Knowledge moves through a pipeline that actively improves it:

```
ingest → extract → synthesize → judge → link → promote
```

- **Extract** structured facts/entities/decisions from raw documents and sessions.
- **Synthesize** higher-order knowledge from many low-level signals.
- **Judge** candidates for quality before they become durable.
- **Link** them into the knowledge graph (§4).
- **Promote** the ones that prove useful; **decay** the ones that don't.

It tunes itself: promotion thresholds are calibrated from outcomes, routing and
ranking policies are learned from feedback, and it runs reflection passes on idle
time to consolidate what it has seen, without you asking.

- **Backed by:** idle-time reflection passes synthesize new knowledge from what
  aimee has seen; Bayesian calibration tunes promotion thresholds from outcomes;
  contextual-bandit routers learn ranking and routing from feedback; and lifecycle
  states plus scheduled maintenance cycles promote what proves useful and decay what
  doesn't.

---

## 4. Pattern recognition across all domains

Everything aimee ingests lands in one typed knowledge graph. Recall is a **hybrid
vector-graph**, not plain vector search: the embeddings, the graph's entities and relations,
and a lexical signal are ranked together by reciprocal-rank fusion, so a query resolves
through meaning, structure, and keywords at once. That is what lets it draw conclusions no
single document states, by connecting entities, edges, and evidence that originated in
different teams, formats, and domains.

This is the core of the company-wide design. A product spec, the code that
implements it, the support tickets about it, and the contract that governs it all
resolve to the same entities in the same graph, so aimee can reason across the
boundary between any two domains. For example:

- Turn a **business/product document** into concrete **engineering implications**
  ("this billing change requires these services to handle proration").
- Answer a deep **technical/implementation** question for a **non-technical
  audience** in plain language ("is the data deletion request actually honored?
  yes, here's where, in terms a lawyer can act on").
- Connect a **support** pattern to the **engineering** root cause, or a **sales**
  commitment to the **roadmap** that has to deliver it.

Business and engineering is one pair; the same machinery spans every domain in the
company.

- **Backed by:** a typed knowledge-graph ontology with case-based recall and
  contradiction logic, PageRank-based context pruning, per-entity profile cards,
  and citation-backed synthesized recall (`memory_ask`) that returns answers with a
  confidence signal.

Answers come with citations and a confidence signal, so a cross-domain conclusion
can be traced back to the documents it was drawn from.

---

## 5. Delegation that cuts cost

A company-scale knowledge base does real work (synthesis, extraction, review, code
changes), and that work costs inference. aimee keeps the bill down by routing each
task to the cheapest model that can actually do it:

- **Local models (Ollama)** cost nothing.
- **Subscription-plan delegates** (ChatGPT Plus, `mistral-plan`) cost nothing
  extra; you've already paid for the seat.
- Work reaches a **pay-per-token** API only when a task needs it.

The router picks the cheapest capable delegate, runs it (in parallel where it
helps), and hands the primary agent a compact result instead of making it process
raw content, so you save both on the delegate and on the primary agent's context.
The economics layer tracks cost and success per delegate so routing improves over
time.

- **Backed by:** the delegate router and economics layer (`src/server/delegate_*.c`,
  `delegate_economics.c`); see [Setting Up Delegates](DELEGATES.md).

```bash
aimee delegate review "Review this PR"        # cheapest capable delegate
aimee delegate code --tools "Add tests"       # write-capable, isolated worktree
aimee agent list                              # configured delegates + slots
```

---

## 6. The foundation it all rides on

| Layer | What it gives the knowledge base |
|-------|----------------------------------|
| 4-tier scoped memory (L0-L3) | Fast scratch to durable facts, with automatic promotion and decay |
| Guardrails | Sensitive-file blocking and anti-pattern detection on the hot path |
| Session isolation | Parallel sessions in isolated git worktrees that never clobber each other |
| Zero-cloud | Runs fully local; hosted inference is opt-in, not required |
| Speed | Sub-10ms session start and hook checks |

See the [Architecture](ARCHITECTURE.md) for how `aimee-server` (hot path, DB1) and
`aimee-kb` (knowledge, DB2/DB3) split this work, and the [Manual](../MANUAL.md) for
the day-to-day commands.

---

## 7. Typed facts: a validated relationship layer

Beyond free-text memory, aimee can record typed facts: relationships with real
semantics, validated before they are stored. "Alice `works_for` Acme" and "the
laptop `device_has_ip` 10.0.0.3" are not just sentences; they are triples checked
against an ontology of what each relation means.

- **An ontology defines each relation.** A `rel_types` table is the single source
  of truth: every relation (`works_for`, `spouse`, `lives_in`, `device_has_ip`, …)
  declares its allowed subject/object kinds (`PERSON`, `DEVICE`, `PLACE`, `ORG`,
  `IP`, `SCALAR`, …), whether it is symmetric, its inverse, a correction policy,
  and a PII sensitivity tier. A seed ontology ships in code (so it works during a
  DB outage); the live table can be extended by operators or grown automatically by
  promotion. Nothing rejects "the printer `works_for` the kernel" today; the
  typed-fact gate does, because the kinds don't match.
- **The write gate is the single commit point.** Every candidate triple, from
  pattern extraction, model rewrite, or the curator, passes through one gate before
  it becomes a stored edge. A known relation with valid kinds is committed; a kind
  mismatch is rejected; a relation that is novel (not in the seed) is staged
  provisionally and counts toward promotion. A relation already active in the live
  ontology is treated as known even if it isn't in the seed. If the write itself
  fails, the gate reports *defer* (never a false success) so the fact is retried,
  not silently lost.
- **Confidence classes track provenance.** A fact a user states is **Class A**; a
  model inference consistent with the ontology is **Class B**; a novel/speculative
  relation is **Class C**. Classes drive decay and durability: speculative facts
  expire if never confirmed; confirmed model facts become durable.
- **Corrections respect authority.** Each relation has a correction policy
  (`supersede`, `hard_delete`, or `immutable`). A model or inferred retraction can
  never delete a user-stated (Class-A) fact; only the user can retract their own
  facts, and a model correction that contradicts the user is refused. Retractions
  can target a specific old value or all values of a relation, and corrected facts
  are archived (retained and auditable), never erased.
- **PII is gated per relation.** Each relation carries a sensitivity tier so
  personal facts can be withheld from contexts that shouldn't see them; an
  unknown/learned relation fails closed to the restrictive tier.

Typed facts are off by default; enable them with `typed_facts_enabled: true` in
config. Validated facts join the same `<aimee-context>` envelope as the rest of the
knowledge base, so recall and injection are unchanged.

---

## Where this is today vs. where it's heading

The substrate is real and shipping: the four-tier memory, the curator extraction
pipeline, the scope lattice, the typed knowledge graph and graph retrieval, idle
reflection, calibration/bandit learning, and delegation all exist in the current
build. The all-domain, whole-company reach, ingesting every team's documents into
one shared graph and synthesizing freely across them, is the direction the
architecture charts. This page describes that destination and the mechanisms
already carrying us there; [Feature Status](STATUS.md) tracks what's live today.
