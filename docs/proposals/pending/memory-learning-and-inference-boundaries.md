# Proposal: unify memory, learning, skills, and inference boundaries

- **State:** PENDING — roundtable-approved 2026-07-20; awaiting project acceptance
- **Parent:** [`core-substrate-and-source-module-boundaries.md`](core-substrate-and-source-module-boundaries.md)
- **Owns:** memory/inference semantics, code-intelligence placement, learning/skills safety,
  response composition, and optional KB synthesis
- **Implementation dependencies:** module descriptors and required core contracts
- **Date:** 2026-07-20

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

Code history is a core memory schema and query capability, not a dependency on the optional `git`
module. Core memory accepts typed code-event/snapshot records from any producer and its reference
fixture. The optional `git` module is one producer that translates repository history into that
public ingest contract; core memory never imports Git headers, symbols, commands, or storage.

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
changes, protect pinned/bundled/user skills, audit every mutation, pass offline evals, and have
exactly one scheduler. The curator-deletion proposal does not authorize a replacement.

Offline benchmarks, datasets, ablations, and regression runners belong to optional `evals`.
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
- {id: 3, tier: mechanical, check: "scripts/check_code_intelligence_ownership.sh --owner memory --core-code-history-schema --generic-code-event-ingest --forbid-core-git-imports --forbid-parallel-registry --require-blast-radius-provider"}
- {id: 4, tier: integration, check: "scripts/test_learning_skills_contract.sh --outcome-evidence --privacy --typed-proposal --approval --snapshot --rollback --protected-skill-invariants --audit"}
- {id: 5, tier: hardware, check: "scripts/test_kb_synthesis_readiness.sh --profiles control,full --when-selected --require-provider-capability kb-synthesis --require-resource-manifest --quality-fixture tests/kb_synthesis/readiness.json --write-contract memory.canonical-change --forbid-direct-memory-storage-access --require-revision-provenance-auth-audit-rollback --require-principal-signed-change-digest --verify-execution-policy-trust-root --forbid-self-authorization --typed-unavailable --absent-from-core"}
- {id: 6, tier: mechanical, check: "scripts/check_module_names.sh --forbid memory-tier-b,agent-eval,bare-synthesis --allow-compatibility-records"}
```
