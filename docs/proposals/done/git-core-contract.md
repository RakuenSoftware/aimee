# Proposal: define the required Git core contract

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived delivered scope (2026-07-26).** This proposal is retained as the historical
> specification for work already delivered. Remaining work is tracked in
> [`git-core-contract-runtime-residual.md`](../pending/git-core-contract-runtime-residual.md).

- **State:** DONE — delivered scope archived 2026-07-26.
- **Parent:** [`aimee-core-capability-contract.md`](aimee-core-capability-contract.md)
- **Delivery dependency:** [`module-runtime-source-ownership-and-build.md`](module-runtime-source-ownership-and-build.md)
- **Owns:** Git's repository boundary, memory-ingest seam, provenance, redaction order, and non-Git behavior
- **Historical cutoff:** `6ce37f53e1f627c19e15fc01f68959f546a5eded`
- **Date:** 2026-07-20

## Decision

`git` is required core: it reads repository state and history and produces repository records, but
it does not own code intelligence. `memory` exclusively owns the code-intelligence schema,
persistence, indexing, embedding, reranking, retrieval, and recall. Git may submit a
principal-scoped, redacted, provenance-bound record only through memory's public ingest contract;
it may not write a memory-owned namespace directly.

This proposal defines a structural contract. Its approval does not claim that the current Git
implementation satisfies the contract, that Git is registered or ready, or that source migration
has begun. The existing `src/modules/git/` tree is historical evidence at the immutable cutoff
above. Slice 3 must land the baseline-aware proposal-ordering gate before any descriptor, generated
build/profile registration, readiness claim, source migration, or runtime implementation change.

## Public boundary

The future public Git provider accepts a principal and repository scope and emits a canonical
repository record to `memory.repository-record.ingest.v1`. Memory is the sole persistence writer.
Missing principal scope, a missing signature, verifier failure, incomplete redaction, or a direct
persistence attempt fails closed with a typed reason. Producer attestation and repository
provenance are separate canonical payloads, both are required, and their signing key identities
must differ. They may use the same vetted algorithm or verifier implementation.

Git assembles the complete record first, including producer attestation and repository provenance.
The fail-closed redactor then scans the entire assembled record. Only the post-redaction record may
cross the memory ingest boundary. A detected denied class, unknown payload class, incomplete scan,
or redactor error denies the entire record; no fragment or partial record is persisted.

A workspace without a Git repository remains usable. Git-only operations return typed
`capability_absent`, distinct from authorization denial. Base workspace, memory, routing, delegate,
and tool behavior may not acquire a hidden Git-repository precondition.

## Compatibility and failure behavior

Legacy entrypoints may remain only as declared forwarding aliases to the canonical contracts and
must carry an expiry. The initial contract declares none. Compatibility code may translate surface
values but may not persist code intelligence, bypass principal scope, weaken provenance, or move
redaction after ingest.

Contract failures are deterministic and typed by the acceptance table below. Structural
validation never substitutes for runtime fixtures: the later implementation slice must turn each
acceptance declaration into an executable fixture before reporting readiness.

## Closed machine schema

The fenced JSON block is the sole machine-readable contract instance. Prose may explain it but may
not override it. The `invariants` array is the single machine-readable source consumed by the Slice
3 handoff. Every object is closed: missing or unknown keys fail. IDs use
`^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$`. Paths are repository-relative, normalized, contained beneath
the configured repository root, and reject empty, absolute, dot, dot-dot, backslash, NUL, and
symlink-escape forms. This slice enforces lexical safety; Slice 3 owns filesystem containment for
future trigger paths that do not exist yet. Arrays use the uniqueness rules described by their field names;
trigger-surface path-bearing arrays are unique by `path`, principal scope entries by the
`(namespace, principal, access)` triple, acceptance entries by both `id` and `kind`, and legacy
aliases by `surface`.

The schema admits only objects, arrays, strings, booleans, null, and integers. Duplicate JSON
members, floating/exponent numbers, and non-finite constants fail before validation. Contract
version `1.0.0` has these exact nested shapes:

- `lifecycle`: `status`, `enforcement_scope`, `approval_evidence`; evidence is null while pending
  or has exactly `run_id`, `artifact_path`, `file_sha256`, and `reviewed_contract_sha256`.
- `historical_cutoff`: `commit`, `ref`, `pinned_after_slice`, and `path`.
- `ownership`: `producer`, `memory_owner`, `code_intelligence_exclusive_to_memory`, and
  `git_may_persist_code_intelligence`.
- `ingest_boundary`: `contract_id`, `namespace`, `writer`, and `git_access`.
- `principal`: `policy`, `read_scope`, `write_scope`, and `code_intelligence_namespaces`; every
  scope entry has exactly `namespace`, `principal`, and `access`.
- each provenance side has one `signature` object with `algorithms` and `verifier_contract`;
  provenance also declares `require_both` and `identity_rule`.
- `redaction`: `policy`, `stage`, `allow_classes`, `deny_classes`, `applies_to`, and
  `audit_contract`.
- `non_git_workspace`: `behavior` and `git_only_operation`.
- every legacy alias has `surface`, `entrypoint`, `canonical_contract`, and `expires`.
- `trigger_surface` contains closed descriptor/build/profile path records, readiness records, and
  status-root records.
- every acceptance entry has `id`, `tier`, `kind`, and an `expected` object containing `decision`
  and `reason_code`.

For every code-intelligence namespace, the union of write scopes contains exactly the memory
writer and never a Git writer. The ingest namespace is one of those namespaces. Redaction allow
and deny sets are disjoint, the deny set includes credentials, tokens, and private keys, and the
applies-to set is exact. The validator binds every mandatory fixture kind to its declared expected
decision and reason code.

```json git-core-contract
{
  "schema_version": 1,
  "contract_version": "1.0.0",
  "module": "git",
  "classification": "required",
  "invariants": [
    "git-required-core",
    "memory-owns-code-intelligence",
    "principal-scoped-ingest",
    "signed-producer-and-repository-provenance",
    "pre-persistence-secret-redaction"
  ],
  "lifecycle": {
    "status": "roundtable-approved",
    "enforcement_scope": "structural-only",
    "approval_evidence": {
      "run_id": "oprun_g6a5e97653a8b0afb_1784587926_15",
      "artifact_path": "docs/validation/roundtable/git-core-contract.json",
      "file_sha256": "77abcd7893d7db8e55caca04ef86c45f3f5ac35c3f5c252f766e444549985d9b",
      "reviewed_contract_sha256": "551fdfb29f68bf4235c0a8b8baf6d5a154697bdc0d7e5c6ce622028732f46470"
    }
  },
  "historical_cutoff": {
    "commit": "6ce37f53e1f627c19e15fc01f68959f546a5eded",
    "ref": "refs/heads/feature/core-modularization",
    "pinned_after_slice": 1,
    "path": "src/modules/git"
  },
  "ownership": {
    "producer": "git",
    "memory_owner": "memory",
    "code_intelligence_exclusive_to_memory": true,
    "git_may_persist_code_intelligence": false
  },
  "ingest_boundary": {
    "contract_id": "memory.repository-record.ingest.v1",
    "namespace": "memory.code-intelligence.repository-records",
    "writer": "memory",
    "git_access": "submit-only"
  },
  "principal": {
    "policy": "default-deny",
    "read_scope": [
      {"namespace": "workspace.repository", "principal": "git", "access": "read"},
      {"namespace": "memory.code-intelligence.repository-records", "principal": "memory", "access": "read"}
    ],
    "write_scope": [
      {"namespace": "memory.code-intelligence.repository-records", "principal": "memory", "access": "write"}
    ],
    "code_intelligence_namespaces": [
      "memory.code-intelligence.repository-records"
    ]
  },
  "provenance": {
    "producer": {
      "signature": {
        "algorithms": ["ssh-ed25519", "openpgp"],
        "verifier_contract": "git.provenance.producer.verify.v1"
      }
    },
    "repository": {
      "signature": {
        "algorithms": ["ssh-ed25519", "openpgp"],
        "verifier_contract": "git.provenance.repository.verify.v1"
      }
    },
    "require_both": true,
    "identity_rule": "distinct-key-identities"
  },
  "redaction": {
    "policy": "fail-closed",
    "stage": "before-memory-ingest",
    "allow_classes": ["source-code", "repository-metadata", "commit-metadata", "signature-metadata"],
    "deny_classes": ["credentials", "tokens", "private-keys"],
    "applies_to": ["record-payload", "producer-attestation", "repository-provenance"],
    "audit_contract": "audit.security-event.append.v1"
  },
  "non_git_workspace": {
    "behavior": "usable",
    "git_only_operation": "capability_absent"
  },
  "compatibility": {
    "legacy_entrypoints": []
  },
  "trigger_surface": {
    "descriptors": [
      {"path": "src/modules/git/module.yaml", "kind": "descriptor"}
    ],
    "generated_builds": [
      {"path": "src/generated/modules.mk", "kind": "make-build"},
      {"path": "cmake/generated/modules.cmake", "kind": "cmake-build"}
    ],
    "generated_profiles": [
      {"path": "build/inventory/core.json", "kind": "generated-profile"}
    ],
    "readiness_markers": [
      {"path": "src/modules/git/module.yaml", "key": "readiness", "value": "ready"}
    ],
    "status_claim_roots": [
      {"path": "docs/proposals", "claim": "git-runtime-ready"},
      {"path": "docs/modules", "claim": "git-runtime-ready"}
    ]
  },
  "acceptance": [
    {"id": "cross-principal-access", "tier": "integration", "kind": "cross_principal_access", "expected": {"decision": "deny", "reason_code": "PRINCIPAL_SCOPE_DENIED"}},
    {"id": "missing-principal-scope", "tier": "integration", "kind": "missing_principal_scope", "expected": {"decision": "deny", "reason_code": "PRINCIPAL_SCOPE_MISSING"}},
    {"id": "missing-producer-signature", "tier": "integration", "kind": "missing_producer_signature", "expected": {"decision": "deny", "reason_code": "PRODUCER_SIGNATURE_MISSING"}},
    {"id": "missing-repository-signature", "tier": "integration", "kind": "missing_repository_signature", "expected": {"decision": "deny", "reason_code": "REPOSITORY_SIGNATURE_MISSING"}},
    {"id": "producer-verifier-failure", "tier": "integration", "kind": "producer_verifier_failure", "expected": {"decision": "deny", "reason_code": "PRODUCER_VERIFICATION_FAILED"}},
    {"id": "repository-verifier-failure", "tier": "integration", "kind": "repository_verifier_failure", "expected": {"decision": "deny", "reason_code": "REPOSITORY_VERIFICATION_FAILED"}},
    {"id": "unknown-payload-class", "tier": "integration", "kind": "unknown_payload_class", "expected": {"decision": "deny", "reason_code": "UNKNOWN_PAYLOAD_CLASS"}},
    {"id": "incomplete-redaction-scan", "tier": "integration", "kind": "incomplete_redaction_scan", "expected": {"decision": "deny", "reason_code": "REDACTION_SCAN_INCOMPLETE"}},
    {"id": "redactor-error", "tier": "integration", "kind": "redactor_error", "expected": {"decision": "deny", "reason_code": "REDACTION_ENGINE_FAILED"}},
    {"id": "unredacted-secret", "tier": "integration", "kind": "unredacted_secret", "expected": {"decision": "deny", "reason_code": "REDACTION_DENY"}},
    {"id": "split-input-secret", "tier": "integration", "kind": "split_input_secret", "expected": {"decision": "deny", "reason_code": "REDACTION_DENY"}},
    {"id": "partial-persistence-prevented", "tier": "integration", "kind": "partial_persistence_prevented", "expected": {"decision": "deny", "reason_code": "PARTIAL_PERSISTENCE_FORBIDDEN"}},
    {"id": "direct-git-persistence", "tier": "mechanical", "kind": "direct_git_persistence", "expected": {"decision": "deny", "reason_code": "GIT_PERSIST_FORBIDDEN"}},
    {"id": "git-owned-code-intelligence", "tier": "mechanical", "kind": "git_owned_code_intelligence", "expected": {"decision": "deny", "reason_code": "MEMORY_EXCLUSIVE"}},
    {"id": "submit-only-violation", "tier": "mechanical", "kind": "submit_only_violation", "expected": {"decision": "deny", "reason_code": "SUBMIT_ONLY_VIOLATION"}},
    {"id": "non-git-workspace-git-only-operation", "tier": "integration", "kind": "non_git_workspace_git_only_operation", "expected": {"decision": "capability_absent", "reason_code": "CAPABILITY_ABSENT"}},
    {"id": "signer-identity-reuse", "tier": "integration", "kind": "signer_identity_reuse", "expected": {"decision": "deny", "reason_code": "SIGNER_IDENTITY_REUSE"}},
    {"id": "deny-allow-overlap", "tier": "mechanical", "kind": "deny_allow_overlap", "expected": {"decision": "deny", "reason_code": "DENY_ALLOW_OVERLAP"}},
    {"id": "post-approval-contract-mutation", "tier": "mechanical", "kind": "post_approval_contract_mutation", "expected": {"decision": "deny", "reason_code": "REVIEW_DIGEST_MISMATCH"}},
    {"id": "valid-repository-ingest", "tier": "integration", "kind": "valid_repository_ingest", "expected": {"decision": "pass", "reason_code": "CONTRACT_SATISFIED"}},
    {"id": "non-git-workspace-base-operation", "tier": "integration", "kind": "non_git_workspace_base_operation", "expected": {"decision": "pass", "reason_code": "CONTRACT_SATISFIED"}}
  ]
}
```

## Approval binding

The validator has explicit `pending` and `roundtable-approved` modes. Pending requires null evidence.
Roundtable-approved requires an external evidence record at
`docs/validation/roundtable/git-core-contract.json`. That record binds the roundtable run and the
SHA-256 of the canonical contract after only the lifecycle status and evidence fields are
normalized back to their pending values. It also binds the exact evidence-file bytes. Consequently,
only an evidence-only lifecycle transition (`status` and `approval_evidence` fields) is permitted;
every other post-review mutation fails.

The contract digest intentionally binds the parsed canonical value, not Markdown whitespace or JSON
key order. Strict parsing rejects duplicate keys, floats, exponent forms, and non-finite values, so
formatting-only changes preserve the digest while every representable semantic change alters it.

The repository evidence is an auditable pointer and content binding, not a self-authenticating
credential. Authorization to merge it comes from the protected pull-request workflow and its
required roundtable review. The checker verifies internal consistency and tamper evidence; it does
not claim that contributor-controlled bytes can prove their own author.

## Trigger surface and ordering

The trigger-surface paths and status claims above are inputs to Slice 3's diff gate; they are not
runtime attestations. For any listed trigger-surface path or status claim, Slice 3 compares the
checked-out event revision to the immutable cutoff, then rejects migration signals unless this child
is approved. The cutoff must resolve as a commit, be an ancestor of the checked-out revision, and
contain `src/modules/git`.

## Non-goals

- Editing or accepting the current Git implementation.
- Registering a Git descriptor, object, profile, stage, route, provider, or readiness state.
- Moving code intelligence, persistence, embedding, reranking, or retrieval out of memory.
- Persisting code intelligence through any path other than `memory.repository-record.ingest.v1`.
- Requiring every workspace to contain a Git repository.
- Choosing final cryptographic providers or freezing one signature algorithm.
- Implementing the repository-wide proposal-ordering gate owned by Slice 3.

## Binding checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "python3 -I -S scripts/check_git_core_contract.py --require-status roundtable-approved"}
- {id: 2, tier: mechanical, check: "python3 -I scripts/tests/test_check_git_core_contract.py -v"}
- {id: 3, tier: integration, check: "future: tests/git_core_contract/run_fixtures.sh --from-embedded-contract --require-all-acceptance-kinds --no-runtime-ready-before-pass"}
- {id: 4, tier: mechanical, check: "future Slice 3: scripts/check_proposal_ordering.sh --contract docs/proposals/pending/git-core-contract.md --cutoff 6ce37f53e1f627c19e15fc01f68959f546a5eded --trigger-surface embedded --must-pass-before-git-migration"}
```
