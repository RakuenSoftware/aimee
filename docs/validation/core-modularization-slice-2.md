# Core modularization slice 2: required Git core contract

## Decision

This slice defines and mechanically validates the child contract required before Git can be bound
to the new module runtime. It does not change Git source or claim runtime enforcement. The embedded
contract in `docs/proposals/pending/git-core-contract.md` is the sole machine-readable contract
instance.

The contract fixes five suite invariants:

1. `git-required-core`: Git remains one of the eighteen required core modules.
2. `memory-owns-code-intelligence`: memory exclusively owns code-intelligence schema, persistence,
   indexing, embedding, reranking, retrieval, and recall.
3. `principal-scoped-ingest`: Git can submit only within the authorizing principal's default-deny
   scope, and memory remains the persistence writer.
4. `signed-producer-and-repository-provenance`: separately signed producer attestation and
   repository provenance are both required and use distinct signing key identities.
5. `pre-persistence-secret-redaction`: the complete repository record is scanned fail-closed before
   crossing memory's ingest boundary.

## Historical boundary

Commit `6ce37f53e1f627c19e15fc01f68959f546a5eded`, the
`feature/core-modularization` tip immediately after Slice 1, is the immutable historical cutoff.
The existing `src/modules/git/` tree at that commit predates this effort and is not a migration
signal. The checker verifies the cutoff object, ancestry to the checked-out event revision, and the
tree path without copying path hashes into a second baseline.

## Slice 3 handoff

```json slice3-handoff
{
  "schema_version": 1,
  "receiver": "slice-3-proposal-ordering-gate",
  "contract_file": "docs/proposals/pending/git-core-contract.md",
  "evidence_file": "docs/validation/roundtable/git-core-contract.json",
  "invariants": [
    "git-required-core",
    "memory-owns-code-intelligence",
    "principal-scoped-ingest",
    "signed-producer-and-repository-provenance",
    "pre-persistence-secret-redaction"
  ],
  "ordering_script_baseline": "6ce37f53e1f627c19e15fc01f68959f546a5eded",
  "trigger_surface_source": "git-core-contract"
}
```

Slice 3 must consume this handoff and the contract's trigger surface to implement
`scripts/check_proposal_ordering.sh`. It is a hard predecessor to every new Git descriptor,
generated Make/CMake registration, generated profile registration, readiness/status claim, source
migration, or runtime implementation change.

## Included

- the Git core proposal and its single embedded JSON contract
- a standard-library, CWD-independent, strict JSON validator
- mutation tests for ownership, principal scope, provenance, redaction, non-Git behavior, trigger
  paths, cutoff pinning, fixture outcomes, lifecycle, evidence tampering, and contract tampering
- the `git-core-contract` CI job on the feature branch and its pull requests
- external roundtable evidence bound to the reviewed substantive contract

## Deferred

- all edits under `src/modules/git`
- `src/modules/git/module.yaml` and any runtime descriptor
- Make/CMake or generated-profile registration
- readiness claims and runtime acceptance fixtures
- the baseline-aware cross-proposal ordering checker
- source movement, API replacement, and memory-ingest implementation

The approval gate proves only structural declaration and review binding. It does not prove that the
current Git code enforces the future contract.
