# Proposal: unify memory, learning, skills, and inference boundaries

- **State:** DONE — archived 2026-08-04 as partially implemented. Source ownership and the memory,
  learning, skills, response-composition, and synthesis boundaries now exist; cross-process bus
  isolation and remaining production-quality contracts continue in
  [`memory-learning-cross-process-boundaries-residual.md`](../pending/memory-learning-cross-process-boundaries-residual.md).
- **Historical state:** roundtable-approved 2026-07-20; awaiting project acceptance
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries-residual.md)
- **Owns:** memory/inference semantics, code-intelligence placement, learning/skills safety,
  response composition, and optional KB synthesis
- **Implementation dependencies:** module descriptors and required core contracts
- **Date:** 2026-07-20 (reconciliation note added 2026-07-23)

> **2026-07-23 amendment reconciliation.** The memory/learning/skills/response-composition semantics
> this proposal owns are unchanged, but the suite amendment
> ([`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries-residual.md))
> changes how `memory` runs: it is a **separate program on the shared-memory event bus**, not an
> in-process C module, and it is the dependency **hub/sink** (suite invariant 14) — nearly every
> module depends on it while it depends on no feature module, and its public surface is a narrow event
> contract (ingest/recall/index/embed/rerank). Code intelligence remains owned by `memory`. Its
> round trip crosses the bus within the committed performance budget rather than as an in-process
> call, and its stages appear as bus events in the core round-trip proof. These are boundary/runtime
> changes, not semantic ones; the proposal is flagged for re-review to confirm no semantic contract
> assumed an in-process memory call.

## Decision

Memory is not useful without population and ranked recall. Structured extraction/indexing,
embedding, and reranking are therefore required memory operations with working core providers.
Code intelligence is memory over code, not a parallel subsystem. Response composition and adaptive
learning are required core behavior. Heavyweight KB synthesis remains optional.

This proposal owns the semantics and quality criteria of stages 6, 8, 10–12, and 20 in the core
round-trip manifest: extraction/indexing, embedding, reranking, skill context, learning observation,
and response composition. The core-contract proposal owns their ordering and end-to-end traversal.

## Memory ownership

The `memory` module owns persistent, episodic, working, semantic, and code memory plus their shared
provenance and retrieval contracts. Symbols, references, call graphs, dependency graphs, code
history, architecture facts, and blast-radius evidence live under memory ownership. Specialist
parsers, OCR, proprietary extractors, additional model backends, and advanced analyzers may be
optional providers; the operations and one working implementation are not.

Code history is a core memory schema and query capability. The required `git` module is a required
producer of repository state/history through memory's public ingest boundary; memory retains code-
event schema, provenance, storage, indexing, retrieval, and code-intelligence ownership. Non-Git
producers may use the same generic ingest boundary but do not displace the required Git reference
implementation. The required forthcoming `git-core-contract.md` child must define the record
contract and security semantics before the Git migration slice begins. Memory may depend only on
that generic ingest contract; it may not import Git headers, symbols, commands, or storage.
Memory owns all tables, schemas, indices, persistence, and retrieval for repository-derived records;
Git writes only through the public ingest boundary.
Additional producers remain providers behind that contract. They become module IDs only by
amending the canonical taxonomy in `core-substrate-and-source-module-boundaries.md`.

Required readiness probes validate:

- structured-extraction schema/grammar, provenance, and non-empty entity/claim output;
- embedding model identity, dimension, cardinality, finite values, and non-constant behavior;
- reranking permutation, score validity, and a non-identity relevance fixture; and
- response-composition production of a schema-valid canonical IR response with evidence citations.

Provider unavailability makes the owning required capability non-ready. Test fixtures are injected
only into the test binary and cannot satisfy production readiness.

Production provider provenance is part of the signed profile manifest. Production profiles reject
test fixture object IDs, descriptors, handles, and registration namespaces before probes run.
Readiness uses nonce-seeded challenge inputs that are unavailable at build time, verifies input-
sensitive extraction/embedding/reranking behavior across perturbations, and requires a provider
work receipt tied to the challenge and production provider identity. Canned outputs, ID-only
reranking, epsilon-only vectors, and fixture counters fail.

## Response composition versus KB synthesis

`response-composition` reads request context and ranked evidence to produce the final response,
summary, and citations. It does not mutate canonical memory and is required in core.

`kb-synthesis` improves canonical shared memory through semantic deduplication, entity resolution,
contradiction reconciliation, and governed promotion/demotion. It is optional because a useful
implementation requires a capable, resource-heavy LLM or substantial GPU. When selected without
adequate resources it reports a typed unavailable state. New code and config may not use ambiguous
bare `synthesis` or the historical `memory-tier-b` name.

KB synthesis cannot access memory storage or mutate records directly. It submits a typed change set
through the core `memory.canonical-change` contract, including expected revision, evidence,
provenance, conflict policy, authorization context, and rollback metadata. Core memory validates,
authorizes, audits, and atomically applies or rejects that change. The optional module owns judging
and proposing; core memory owns canonical integrity and the write transaction.

The authorization context contains a principal-signed change digest verified against the core
execution-policy trust root. KB synthesis cannot mint, substitute, or self-authorize that signature;
policy evaluation binds principal, tenant, evidence digest, requested mutations, and expiry.

## Learning and skills

Learning the user is part of Aimee's core mission. `learning` owns evidence-backed preferences,
corrections, working style, successful and failed approaches, privacy controls, provenance,
reversibility, and outcome feedback. It may propose changes to procedural memory only through the
public `skills` contract.

`skills` owns discovery, validation, matching, application, provenance, safe updates, snapshots,
and rollback. Individual skill packages remain optional content. Production learning may not
silently rewrite protected, pinned, bundled, or user-authored skills. Mutations require a typed,
user-visible proposal, approval policy, audit event, snapshot, and rollback path.

If background skill curation is proposed again, this proposal owns its admission contract. A
replacement must use real activation/outcome evidence, include project and user scope, use memory
embedding/reranking, emit a typed user-visible proposal, require approval, snapshot and roll back
changes, protect pinned/bundled/user skills, audit every mutation, pass offline benchmarks, and have
exactly one scheduler. The curator-deletion proposal does not authorize a replacement.

Offline benchmark runners, datasets, ablations, and regression suites belong to optional
`benchmarks`.
Production adaptation belongs to `learning`; memory quality fixtures belong to memory test support.
There is no mixed `agent-eval` module.

## Non-goals

- Making KB synthesis necessary for memory or response construction.
- Moving response composition into presentation code.
- Treating skill packages as architectural modules.
- Preserving historical inference names indefinitely.

## Binding checks

```yaml acceptance
- {id: 1, tier: integration, check: "scripts/test_memory_inference_contract.sh --extraction-required --embedding-required --reranking-required --typed-readiness --signed-production-provider-manifest --forbid-fixture-objects-handles-descriptors-namespaces --nonce-challenges --require-input-sensitive-work-receipts --fail-canned-id-only-epsilon-providers"}
- {id: 2, tier: integration, check: "scripts/test_memory_quality_fixture.sh --require-tier-a-schema-valid --require-tier-a-grammar-conformant --min-cosine-margin 0.10 --min-distinct-l2 1e-6 --require-rerank-order m3,m1,m2 --min-ndcg 0.95 && scripts/test_response_composition_contract.sh --ranked-evidence --canonical-ir --citations --no-memory-mutation"}
- {id: 3, tier: mechanical, check: "scripts/check_code_intelligence_ownership.sh --owner memory --core-code-history-schema --generic-code-event-ingest --forbid-core-git-imports --forbid-git-memory-storage-ownership --forbid-parallel-registry --require-blast-radius-provider"}
- {id: 4, tier: integration, check: "scripts/test_learning_skills_contract.sh --outcome-evidence --privacy --typed-proposal --approval --snapshot --rollback --protected-skill-invariants --audit"}
- {id: 5, tier: hardware, check: "scripts/test_kb_synthesis_readiness.sh --profiles control,full --when-selected --require-provider-capability kb-synthesis --require-resource-manifest --quality-fixture tests/kb_synthesis/readiness.json --write-contract memory.canonical-change --forbid-direct-memory-storage-access --require-revision-provenance-auth-audit-rollback --require-principal-signed-change-digest --verify-execution-policy-trust-root --forbid-self-authorization --typed-unavailable --absent-from-core"}
- {id: 6, tier: mechanical, check: "scripts/check_module_names.sh --forbid memory-tier-b,agent-eval,evals,bare-synthesis --allow-compatibility-records"}
```
