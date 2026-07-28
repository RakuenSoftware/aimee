# Aimee-native knowledge model: versioned sources and external representation integration

Status: implementation in progress

## Scope and repository boundary

This public proposal covers Aimee's runtime responsibilities for versioned evidence and an external
learned-representation provider. The provider is an opaque, independently versioned component.
Its model architecture, tokenizer construction, datasets, curation, training objectives, teacher
supervision, evaluation corpus, thresholds, checkpoints, and operating procedures are private model
artifacts and are intentionally outside this repository.

Aimee owns immutable evidence, source monitoring, typed structure, access control, provenance,
freshness, deterministic authority enforcement, and atomic index publication. The external provider
may supply compatible representations and relevance signals. Public integration records only the
metadata required to validate an artifact: provider/model identity, checkpoint and tokenizer hashes,
representation schema, output compatibility, health, and generation identity.

## Runtime invariants

1. Complete original source bytes are primary evidence and remain content-addressed and immutable.
2. Chunks and extracted code units are secondary evidence. Each names one exact original version and
   carries source anchors. Chunks are never concatenated to reconstruct the original.
3. Representations, graph edges, extracted claims, summaries, memories, judgments, and syntheses are
   tertiary evidence. Every tertiary object retains lineage through its secondary parent to one
   exact primary version. Lower-authority evidence never overrules higher-authority evidence.
4. Repository identity is independent of checkout path. Ref, commit, tree, snapshot, and index
   generation are distinct identities.
5. A default branch is a moving selector, not a storage namespace. Non-default branches coexist.
6. Committed snapshots are immutable. WIP/worktree evidence is explicitly labeled and cannot replace
   committed evidence.
7. A generation becomes queryable only through atomic publication after every required source,
   structural, representation, and index validation succeeds.
8. Exact lexical retrieval remains independent of learned signals.
9. Staleness is observable. Stale evidence is never silently described as current.
10. In a Git workspace, an omitted project means the attached checkout's exact ref and commit; it
    never means an unbounded union of projects or branches. Detached HEAD fails closed.
11. Deleted/merged refs and superseded document renditions leave current retrieval immediately.
    Immutable originals remain available only under the applicable historical-retention policy.
    A ref that is the caller's attached checkout is live work and is never retired underneath it:
    a branch created off the default and a branch fast-forward-merged into it are indistinguishable
    from refs alone, so the checkout is what separates unstarted work from finished work.
12. Every derived object inherits the access scope of the evidence it derives from and can never
    widen it. Repository identity is scoped to a tenant, because a repository key is derived from
    the repository itself and is therefore identical for two tenants indexing the same upstream.
13. Recovery from a stale generation requires the authority to publish one. A caller without it is
    told what is published instead of being asked to retry an action it cannot perform.
14. Output that fails validation never enters the knowledge base. Retrieval runs before a request is
    served; validation runs before anything derived from a response is persisted. A response that is
    refused, aborted, or fails a guardrail leaves no trace in memory, and a rejection is recorded as
    a rejection rather than as an absence.
15. Condensed context is an index into evidence, never a replacement for it. Every condensation
    names the exact records it covers and can be resolved back to them.

## Evidence authority and lineage

Authority is ordinal and cannot be compensated for by similarity or confidence:

| Tier | Evidence | Runtime rule |
| --- | --- | --- |
| Primary | Exact original bytes at a revision or commit | Immutable final citation target |
| Secondary | Anchored chunk, code unit, page region, cell, or rendition | Must name one primary version and carry anchors |
| Tertiary | Representation, graph edge, claim, summary, memory, judgment, or synthesis | Must name its secondary parent and exact primary lineage |

Primary evidence establishes what the source contains. Secondary evidence locates and parses it.
Tertiary evidence helps retrieve or interpret it. Before presentation, evidence escalates to the
primary version and exact anchors. Contradictory primary evidence invalidates lower-tier claims;
contradictory secondary evidence suppresses derived interpretation until reconciled. Multiple
tertiary descendants of the same source span count as one provenance root.

`kb_evidence_lineage` is the cross-surface ledger for subject, tier, exact original, secondary parent,
anchors, derivation identity, hashes, and generation. Specialized indexes retain their efficient
layout, but no derived output is publishable without complete lineage.

## Typed structural graph

Graph construction is Aimee's responsibility, not the provider's. The runtime builds and versions
typed layers over indexed nodes: syntax/symbol, call/dataflow, dependency/build, test/behavior,
change/history, repository/provenance, document/evidence, and semantic-neighbor relations. Each edge
is scoped to one snapshot and generation and carries the lineage required of its tier.

The first seven layers are deterministic: they are derived from parsed source and recorded history,
and they are reproducible from the same snapshot. The semantic-neighbor layer is vector-derived and
therefore tertiary. A semantic edge never becomes a structural fact. It may expand a candidate set
and it may explain why a candidate surfaced, but it cannot establish that a symbol is defined,
called, built, tested, or superseded, and it cannot feed back into a first-pass representation.

Because deterministic edges are computed before representations are requested, the runtime can offer
each indexed node a relation-balanced context envelope without depending on the provider. Content
representations are a function of source content and model version only; context representations
carry a separate hash and may depend on typed neighbors, authority, validity, and snapshot. The two
are stored and invalidated independently.

## Document identity

- `kb_original_documents` stores logical source identity.
- `kb_original_versions` stores immutable hash, byte length, media type, blob reference, capture
  time, and source revision where available.
- `kb_document_heads` selects the current published version of each logical document.
- `kb_document_version_lifecycle` records publication and supersession without mutating old primary
  evidence.
- `kb_documents` stores derived chunks tied to one exact original version and rendition/parser.

The content-addressed blob store retains originals. Re-ingest can replace derived indexes without
discarding the prior original. Only a successful, non-empty derivation advances the document head.
Source revision or ETag participates where trustworthy; full content hashing remains final identity.

## Repository and branch identity

- `kb_source_repositories` stores checkout-independent repository identity and configured default
  ref.
- `kb_source_refs` stores a moving ref, observed head/tree, active snapshot, publication state, and
  active/merged/deleted lifecycle.
- `kb_source_snapshots` stores immutable commit/tree or explicitly labeled WIP identity.
- `kb_index_generations` stages and publishes one snapshot's index while recording compatible
  provider-artifact metadata and validation results.

Every indexed path, symbol, edge, memory fence, and external representation is scoped to a snapshot
and generation. Cross-branch queries are explicit unions. A single-branch query cannot leak another
snapshot's candidates.

Repository identity is also scoped to a tenant. `repository_key` is derived from the repository, so
two tenants indexing the same upstream produce the same key; uniqueness is therefore per tenant and
a repository row carries its owning team. Untenanted rows (team 0) are the single-tenant case.
Database-layer enforcement of that boundary — row-level security on the source tables, and a write
path that stamps the authenticated team rather than 0 — is outstanding; it depends on the request
context carrying a team, which today it does not, and lands with the tenant tier's isolation gate.

## Current-checkout routing

The attached checkout is resolved locally using Git's symbolic ref plus exact commit and tree. Aimee
then resolves repository/ref/commit to one physical published generation. A stale request receives a
conflict response; the client publishes the newly observed generation once and retries. It never
substitutes the default branch. An actively checked-out branch remains valid even when its tip is
reachable from the default branch.

This contract applies to symbol lookup, lexical and hybrid code search, callers, structure, blast
radius, graph navigation, code-span reads, source-derived memory, question answering, recall, agent
context, and delegated worktree context. General user/language memory remains shared. Source-derived
memory carries an exact repository/ref/commit/generation fence and is invisible without matching
source context; pruning leaves the fence as a tombstone so derived memory cannot become global.

The committed generation is authoritative for its snapshot. Current worktree bytes and source
packets are a higher-freshness primary overlay for reads and edits; uncommitted bytes are never
mislabeled as part of the committed generation.

## Condensed context and durable state

A long-running session outgrows any context window, so the runtime condenses its own history. This
is an evidence-producing process and obeys the same authority rules as any other.

Condensation is tiered. Raw interaction records are the retained evidence. When they exceed a
budget, a background pass replaces a span of them in the working context with a dense condensation
of that span; when condensations themselves exceed a budget, a further pass condenses those. Each
condensation is tertiary evidence: it names the exact record range it covers, retains lineage to
that range, and can always be resolved back to the underlying records. Dropping a span from the
working context never deletes it, and a condensation never becomes the citation target — a caller
that needs what was actually said reads the records the condensation points at.

Because a condensation is a model output, it is subject to the untrusted-output rules below and to
invariant 14: a condensation that fails validation is discarded and the raw span stays in context.

Promotion between tiers is driven by a measured budget, not a message count, and is designed so the
transition is gradual rather than a cliff:

- each tier has an explicit token budget, measured rather than estimated from message counts;
- condensation is precomputed in the background at a fraction of the budget, so the common case is
  already prepared when the budget is reached;
- a hard ceiling above the budget forces condensation synchronously when background work has fallen
  behind, so the context cannot silently overrun;
- the interval, budget, ceiling, and whether a given condensation was precomputed or forced are all
  observable, because a forced condensation is a latency and quality event worth seeing.

Caller-supplied size hints for opaque parts may inform a budget but never determine it. A hint is
untrusted input, and a budget decision made from an unverified count is a denial-of-service surface.

The same pass that condenses also extracts durable facts, rather than paying for a second reading of
the same records. Extraction is schema-bound, and an extracted fact carries the lineage of the
records it came from like any other tertiary object. Extracted state is stored as state and injected
as state; it does not rewrite the stable prefix of a prompt, so a volatile fact cannot invalidate a
cached prefix on every turn.

Durable state and transcripts have different lifetimes and different scopes:

- transcript scope is the conversation: the ordered records of one exchange;
- durable scope is the principal: facts that outlive any single conversation.

Delegation follows from that split. A delegated agent receives a fresh conversation identity and a
derived, deterministic principal identity, so it inherits durable facts without inheriting the
parent's transcript, and its own records are attributable to the delegation rather than merged into
the parent's history. Source-derived state remains fenced to its repository/ref/commit/generation
regardless of scope; the split governs conversational state, never source authority.

Structured durable state is schema-validated and updated by field-level merge. Whole-document
replacement requires a writer to restate everything it did not intend to change, which silently
discards concurrent updates from another session or delegate.

## Continuous refresh

Events reduce latency and reconciliation provides correctness:

- `post-checkout`, `post-commit`, and `post-merge` hooks asynchronously publish the exact attached
  checkout;
- synchronous stale-generation repair is the backstop when a hook is absent or incomplete;
- filesystem and remote-source events enqueue content checks;
- periodic ref/content reconciliation catches missed events, watcher overflow, clock skew, and
  downtime;
- each queue is idempotent by source identity plus observed revision;
- retries resume staging work and never publish partial generations.

Health exposes observed and published revision, lag, last check, last success, failure reason, and
freshness objective. Retrieval returns generation and freshness metadata and fails closed when policy
does not allow stale evidence.

Elapsed time is part of that metadata, not only source revision. When a session resumes after a
meaningful pause, the gap is marked explicitly in the context rather than left implicit, because
retained context reads as continuous no matter how long ago it was written, and a reader that cannot
see the gap will treat a stale assumption as a current one. This is invariant 9 at the context
layer: the passage of time is evidence about freshness, and suppressing it is a silent staleness.

## Incremental publication

1. Observe and preserve a new ref or document revision.
2. Diff content identities against the prior published snapshot.
3. Reuse safe parse/index/provider outputs for identical content and compatible artifact hashes.
   Content representations may be reused on identical content alone. Context representations may
   not: they depend on typed neighbours, authority, validity, and snapshot, so identical content
   whose neighbourhood moved is context-invalidated and re-requested in step 6.
4. Process added or changed units and tombstone removed units in staging.
5. Rebuild deterministic structural relationships for the affected closure.
6. Request compatible external representations for changed or context-invalidated subjects.
7. Validate source coverage, lineage, hashes, snapshot isolation, freshness, and representation
   compatibility.
8. Atomically supersede the old generation and advance the ref/document selector.

A failed generation leaves the previous published generation intact.

## Finite retention

A merged or deleted ref is retired and detached from ordinary retrieval immediately. Its generations
receive a configurable grace deadline. A bounded worker claims eligible generations, purges their
relational and external projections behind generation fences, and either finalizes or releases the
claim for crash-safe retry. Superseded generations no longer referenced by active selectors are also
retention candidates, preventing update-heavy branches from growing forever.

Document supersession follows the same selector/evidence split. Older original bytes remain primary
historical evidence; derived chunks, vectors, and tertiary projections leave current retrieval after
a newer version publishes.

## Retrieval execution

Query execution is ordered so that access and source scope constrain everything downstream:

1. resolve caller access scope and the exact repository/ref/commit/generation being queried;
2. assemble a bounded candidate union from exact lexical retrieval, any installed learned-sparse
   leg, dense nearest neighbours, and typed structural graph expansion, under a per-leg candidate
   budget and a total union ceiling, both configured rather than implied;
3. widen each match to a bounded window of its neighbours — adjacent chunks of the same document,
   adjacent records of the same exchange — because a match is usually a fragment of the thing the
   caller needed, and adjacency is the sequential counterpart of typed graph expansion;
4. rescore that union with the provider's latent signals where available;
5. rerank a small final set with the provider's cross-encoder;
6. apply deterministic authority, provenance deduplication, and freshness policy;
7. escalate every surviving candidate to its primary version and exact anchors for presentation.

Steps 4 and 5 are optional and reorder only within the union produced by steps 1 to 3. They cannot
add a candidate that access, snapshot, or freshness policy excluded, and they cannot reorder around
step 6, which always runs last and is independent of any learned signal. Adjacency in step 3 widens
within the same document or exchange only; it is not a path to a record the caller's scope excluded.

The reranking input is an evidence packet, not raw text: original document identity, secondary
anchors, the graph path that produced the candidate, evidence tier, authority, valid and observed
time, contradiction state, and freshness. Supplying that packet is a runtime obligation. A provider
that cannot consume it degrades to the deterministic path rather than scoring without provenance.

When a budget truncates a leg, the truncation is reported with the result rather than presented as
exhaustive. A ranking stage that receives fewer candidates than it asked for is a recall change, and
a caller cannot see that from the results alone.

Exact lexical retrieval is always present in the union and is never gated on provider health. When
the provider is absent, unhealthy, or incompatible, steps 4 and 5 are skipped and the deterministic
path serves the query with its quality recorded, not silently degraded. The same obligation applies
to the runtime's own context injectors: losing source context because no checkout resolved, or
because no generation is current, is a degradation and is logged as one. It is never allowed to look
like an ordinary empty result.

## External representation ABI

The runtime representation record contains only integration metadata:

- subject kind and ID, access scope, repository/snapshot/generation where applicable;
- original version and anchors where applicable;
- provider/model/checkpoint/tokenizer/profile and representation-schema identity;
- content and context hashes;
- opaque compatible dense, sparse, latent, or score payloads supported by the installed provider;
- authority, valid time, observed time, freshness, provenance, and publication state.

The provider is an untrusted input boundary, not a trusted subsystem. It is independently versioned
and may be third-party, misconfigured, or compromised, so its output is validated before it is
stored and again before it influences a result: declared schema and profile must match the installed
contract, payload dimensions and lengths must match the declared schema, sizes and counts are
bounded, and scores must be finite and within range. Output that fails validation is rejected and
the subject falls back to the deterministic path; it is never stored, and a rejection is a health
signal rather than a silent skip. A provider cannot widen access, assert provenance, introduce a
subject the runtime did not ask about, or emit a structural claim — its influence is confined to
ordering candidates the runtime already admitted.

Existing specialized indexes remain the fallback during migration. New provider outputs are
shadow-written first; production activation requires public runtime safety gates plus the provider's
private acceptance process. A learned score can reorder only within the candidates allowed by access,
source, freshness, and evidence-authority policy. Deterministic authority enforcement runs last.

## Rollout

1. Implemented: immutable originals, document heads, source snapshot/ref/generation metadata,
   generation-staged branch scans, current-checkout routing, source-fenced memory, ref retirement,
   bounded generation pruning, and PDF supersession.
2. Implemented: current-generation fencing across lexical, structural graph, vector, and memory
   retrieval while preserving explicit cross-project/history queries.
   Both stages build clean and pass the repository unit-test suite, including
   `unit-test-source-generation`, which covers snapshot/ref/generation identity and publication.
   No provider is installed, so every claim above is verified against the deterministic path only.
3. Next: define and test the versioned external representation ABI without importing private model
   construction or training dependencies into Aimee. The retrieval-execution ordering and evidence
   packet above are the runtime side of that ABI and are specified but not yet exercised end to end.
4. Shadow-write provider outputs and validate source reuse, context invalidation, isolation,
   provenance, and rollback.
5. Gate provider-assisted retrieval against the existing deterministic fallback.
6. Atomically promote a complete provider/index generation while retaining the previous compatible
   pair for rollback.
