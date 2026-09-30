# Agent Memory Atlas review: ownership and reliability

Reviewed on 2026-09-29 against `6cd136947`, fetched from `origin/testing` for this review.
The [Atlas Aimee entry](https://neoneye.github.io/agent-memory-atlas/systems/aimee/) reports the
same source pin in its September 28 update. This is a source and documentation review, not a
production security test or a retrieval benchmark.

## The useful contribution is testing the deployed boundary

The Atlas highlights authority-preserving writes, review, persistent rejection, and paired positive
and negative tests. Its most actionable concerns are a role mismatch in rejection-record privileges,
a normalization mismatch between application and database checks, stale capability documentation,
and decision reminders that do not enter model context. It also distinguishes write screening from
recall-time instruction filtering. Its full-stack deployment description should not be read as the
minimum requirements for standalone Server. These observations motivate the independent source
checks below; the external article is not the authority for this checkout.

## Server and KB need separate descriptions

The local [Server composition](../../server-go/modules/server/role.go),
[KB composition](../../server-go/modules/kb/role.go), and
[placement validator](../../server-go/modules/memory/scope.go) establish two roles sharing one
implementation. [Default Compose](../../compose.yaml) deploys Server, PostgreSQL, and embedding,
with no KB. [KB Compose](../../compose.kb.yaml) selects a separate role. Server's
[store selector](../../src/server/server_memory.c) defaults ordinary memory operations to personal
storage and requires `store=kb` to select shared memory.

The previous architecture write path sent every memory operation to KB. The knowledge guide said
archived records remained recallable until two flags were enabled. The retrieval guide named a
removed native candidate-fusion file and described an embedded model as the default deployment.
Those descriptions conflict with the local code. The updated guides make the
[Server/KB boundary](../SERVER_AND_KB.md) canonical and describe current Go retrieval separately
from old evaluation results.

## Findings in the upstream baseline

### 1. Test rejection privileges using the configured SQL role

**Priority: high. Source-confirmed grant mismatch; live exploit not tested.**

[Compose credential provisioning](../../scripts/compose-vault-init.py) selects
`aimee_store_runtime` for `AIMEE_STORE_URL`. The `$memory_store_grants$` block in
[the KB schema](../../src/modules/kb/c/schema.sql) grants that role `SELECT, INSERT, UPDATE,
DELETE` on `memory_rejection_tombstones`, `memory_evidence_events`, and `memory_provenance`.
The [governance SQL gate](../../scripts/memory-governance-pg-test.sql) tests the refusal table
under `aimee_kb_runtime`, whose narrower grants come from
[schema_grants.sql](../../src/modules/kb/c/schema_grants.sql).

The [Go runtime-role replay](../../server-go/modules/memory/runtime_role_test.go) uses
`aimee_store_runtime`, but its existence does not prove the separate no-erasure assertion for that
role. A grant permits an operation subject to RLS and other enforcement; this finding does not
claim unrestricted access to all rejection rows.

Recommended change: remove refusal erasure privileges from every deployed runtime role and keep
restoration as an attributed update. Reconcile existing grants during upgrades. Review evidence
retention separately: an erasable detailed ledger and an immutable content-free audit intent have
different purposes and should not receive identical grants by convenience.

Acceptance: authenticate as the actual provisioned runtime role, verify effective inherited grants,
attempt refusal `DELETE` and `TRUNCATE`, and assert denial. In the same fixture, verify allowed
store, reject, attributed restore, and recall so a blanket denial cannot pass. Repeat after applying
the upgrade twice and through the Go provider, not only `SET ROLE` in an owner connection.

### 2. Give rejection guards the same identity as the fact owner

**Priority: high. Source-confirmed comparison mismatch; bypass not executed.**

[factIdentity](../../server-go/modules/memory/fact_identity.go) normalizes Unicode, case, whitespace,
and relation names. [factTombstoned](../../server-go/modules/memory/fact_mutation.go) checks both
raw triples and recomputed canonical identities, paging through active fact refusals. The
`fact_rejection_tombstone_guard` database trigger in
[the schema](../../src/modules/kb/c/schema.sql) compares raw `source`, `relation`, and `target`.
It protects exact reassertions, but does not enforce the same equivalence relation as Go.

Recommended change: persist a versioned canonical refusal identity and index it. Define one
normalization contract across writers and the database backstop, with explicit treatment of legacy
rows and normalization collisions. Avoid approximating Go's Unicode folding with SQL `lower()`.
An indexed identity also removes the need to scan every active refusal for each fact assertion.

Acceptance: reject a fact, then attempt exact, recased, full-width, and whitespace variants through
both the Go owner and a permitted direct database writer. Keep an unrelated fact as the positive
control. Verify restore attribution, concurrent assert/reject, migration replay, and old rows.

### 3. Separate stored-content screening from instruction trust

**Priority: medium. Current write screening has a narrower contract.**

[screenMemoryWrite](../../server-go/modules/memory/content_gate.go) rejects sensitive keys and
redacts or rejects credential-bearing content. It is not a general instruction-injection classifier.
[Server recall](../../src/server/server_memory.c),
[KB recall transport](../../src/modules/kb_client/kb_client_memory.c), and
[pre-injection](../../src/server/ingress_preinject.c) call `integrity_ingress_decide` when materializing
context. Document ingestion and learning have additional call sites. A stored row is therefore not
proof of safe prompt material.

Recommended change: specify a shared admission contract for model-authored durable instructions,
with explicit reject/quarantine receipts and source authority. Preserve serving-time checks because
old rows and imported evidence remain possible inputs. Do not silently apply a new heuristic to all
user facts without measuring false positives.

Acceptance: exercise personal and KB store, same-key replacement, supersede, extraction, and recall.
Pair malicious instructions with benign quoted examples and ordinary user preferences. Verify that
unavailable checks cannot yield an unqualified successful admission and that recall never promotes
evidence into higher-authority instructions.

### 4. Replace flag-based claims with owner behavior

**Priority: documentation now; automated drift checks next.**

[currentMemorySQL](../../server-go/modules/memory/eligibility.go) applies active lifecycle,
suppression, and half-open valid-time checks directly. The two native lifecycle accessors have no
production caller beyond their definitions in this checkout. The historical
[flag rollout report](../validation/flag-rollout-readiness.md) named `memory_core_helpers.inc`,
which is absent. The knowledge guide repeated that obsolete behavior as current guidance.

The updated guide removes the opt-in-archival claim. The historical report keeps its old results
with a dated correction. Next, tie maintained capability claims to descriptor entries and behavior
tests. A generated config key alone cannot prove there is a live consumer.

Acceptance: default-config recall returns an active positive control while excluding archived,
rejected, suppressed, expired, and not-yet-valid records. Run through public transport as well as
the owner. CI should reject links to removed implementation files and claims of activation for
unused settings.

### 5. Complete the user-visible correction workflow

**Priority: medium. Backend review exists; no correction-review UI caller found.**

[KB correction proposals](../../server-go/modules/memory/correction_proposals.go) and
[personal proposals](../../server-go/modules/memory/personal_proposals.go) retain a model's draft
without replacing protected content. The
[review command](../../server-go/modules/memory/public_correction_proposals.go) requires verified
user authority, exact expected version, proposal ID, and payload digest. The
[Server adapter](../../src/server/server_memory.c) routes both placements; these review operations
retain a KB default and require explicit `store=user` for personal review.

Searches for `review_correction` and `correction-review` under `frontend` and `control-web` find no
caller. This is a missing integration in the inspected UI, not a missing backend review primitive.
Separately, [typed-fact review](../../server-go/modules/memory/fact_review.go) already implements
operator approve, reject, and undo through the
[KB console adapter](../../src/kb/http/kb_http_console.c). Those fact actions do not complete the
memory correction-proposal journey.

Recommended change: expose the existing correction contracts in a review interface with source/draft
comparison, selected store, exact target version, reviewer attribution, and terminal decisions.
Reuse the backend; do not build a second proposal queue.

Acceptance: exercise the UI through the authenticated API. A model cannot approve its own proposal
or borrow user authority; stale revisions conflict; repeated rejected drafts cannot reopen terminal
decisions. The existing [KB tests](../../server-go/modules/memory/correction_proposals_test.go) and
[personal tests](../../server-go/modules/memory/personal_proposals_test.go) provide owner-level cases
to carry across the UI boundary.

### 6. Make decision reminders an explicit product choice

**Priority: medium. No model-context delivery found in the traced memory path.**

[decision_log.c](../../src/modules/kb/c/decision_log.c) marks due decisions `revisit_due`;
[the curator drain](../../src/modules/kb-synthesis/kb_curator_drain.c) runs the sweep, and
[governance HTTP](../../src/kb/http/kb_http_governance.c) lists that state. Searching the Go memory
module finds no `revisit_due` consumer. This supports an operator-list behavior, not automatic
inclusion in a model's next turn.

Recommended change: either present due decisions as an operator queue with that limitation, or
add a scoped, budgeted context channel with source attribution, explicit acknowledgment, and
expiry. Avoid returning every due decision on every turn.

Acceptance: a due decision appears only to its authorized audience, a future decision stays out,
and acknowledged or superseded decisions stop resurfacing. Verify the actual assembled context.

### 7. Join detailed evidence to durable audit delivery

**Priority: medium; define the receipt before promising end-to-end audit.**

The `evidence_object_mutation` trigger in [the schema](../../src/modules/kb/c/schema.sql) writes
a memory mutation intent in the row transaction. The
[WORM worker](../WORM_WORKER.md) later appends committed intents to the separate SQLite chain.
[Go action publication](../../server-go/modules/memory/mutation_audit.go) is an additional
observation path. Successful ring publication, PostgreSQL commit, and completed WORM delivery are
different milestones.

Recommended change: expose a stable correlation from mutation/change identity to outbox intent,
delivery receipt, and chain evidence while keeping content out of immutable metadata. Document
what remains provable after detailed evidence is lawfully erased. Check existing worker receipts
before adding another ledger.

Acceptance: follow a canary through commit, worker restart, idempotent redelivery, and chain
verification. Inject an outbox failure and require mutation rollback; inject a worker outage and
require visible pending delivery without claiming the database transaction rolled back.

## The latest code changes the documentation baseline

The latest tree has retired DB2 and native libpq ownership. Both roles use the Go PostgreSQL
provider; the KB schema is under `src/modules/kb/c/`. The local
[eligibility policy](../../server-go/modules/memory/eligibility.go) is `current-validity-v18`, including
utility-horizon and derived-input checks. The horizon mode remains disabled by default; implemented
policy is not evidence of deployment enablement.

Personal retained revisions, correction proposals in both placements, and source checks before
provider dispatch also exist. The older guide text that called those future work has been updated.
[Release preparation](../validation/release-0.4.6-preparation-2026-09-27.md) records qualification
conditions. Neither an Atlas capability mark nor a passing owner unit test certifies all placements,
transports, runtime roles, and deployed configurations.

## Original implementation priorities

1. **Publish accurate ownership and behavior.** This documentation change supplies the canonical
   boundary, corrected lifecycle/embedding guidance, and links from entry points.
2. **Close the two database gaps.** Runtime refusal privileges and canonical trigger identity have
   concrete local evidence and narrow acceptance criteria.
3. **Finish review and admission contracts.** Reuse the current MR-02 owner contracts and complete their
   UI and public-boundary coverage.
4. **Improve operator evidence.** Deliver decision reminders deliberately and correlate audit
   milestones. Add behavior-backed documentation checks to prevent the same drift.

## Changes included in this PR

| Finding | Resolution |
| --- | --- |
| Runtime refusal erasure | Schema replay and current role provisioning revoke DELETE and TRUNCATE from the configured store role and PUBLIC. Actual-login regression verifies denial and attributed restore. |
| Canonical rejection | A versioned database function supplies the indexed generated refusal identity to both Go lookup and the direct-writer guard. Existing rows are backfilled; equivalent historical refusals remain separate. |
| Admission versus instruction trust | Shared bounded admission refuses direct model-authored instruction overrides before durable writes. Quoted evidence and verified user text remain admissible; serving-time integrity checks remain mandatory. |
| Obsolete capability claims | Current guides link to owning implementations and behavior tests, checked by the existing documentation link gate. Historical evaluation results are explicitly dated. |
| Correction-review UI | Memory Center exposes proposals, source/draft comparison, exact version and digest, explicit placement, authenticated decisions, conflicts, and reviewer attribution. |
| Decision reminders | Governance opens the due operator queue and can close a review by marking the decision superseded. The UI explicitly states that this is not automatic model-context delivery. |
| Audit correlation | Shared-memory intents now carry the individual evidence event ID as well as the changeset. The WORM guide supplies a read-only join to pending/delivered receipts and chain event identity. |

The rejection identity requires the project's PostgreSQL 18 baseline. It uses NFKC,
Unicode full case folding, explicit whitespace collapse, and the owner's bounded ASCII
relation normalization. Invalid or overlong canonical identities retain the raw-triple
fallback. The generated column prevents writers from supplying a false identity. Its
index is deliberately nonunique: normalization must not discard refusal attribution.
Updating the normalizer in future requires a new version and a stored-column migration.

The instruction check is intentionally bounded, not a general injection classifier.
Database-role controls do not constrain a database administrator. UI tests exercise the
API contract with mocked transport; they do not certify a deployed authentication stack.
Audit correlation does not replace worker restart/redelivery or chain-verification tests.
No production database, deployment, or retrieval-quality benchmark was changed or run.

## Validation performed

- The Go memory unit suite passed. PostgreSQL 18 replay passed for the complete runtime
  role path, fact mutations, both correction-proposal owners, public store admission,
  actual runtime-login refusal privileges, and audit correlation.
- An existing refusal survived upgrade with a populated canonical identity. DELETE and
  TRUNCATE remained denied after two full schema replays. This was a disposable local
  database with UTF-8 encoding and UTC session time, not a production migration.
- Frontend tests cover both review placements, exact decimal-string versions, stale
  revisions, backend conflicts, store-switch response isolation, and closing due decisions.
  Runtime and console production builds passed.
- Documentation generation left generated pages unchanged. The full documentation check
  passed in a clean PR checkout. Untracked user drafts were excluded from the PR.

The migration adds a stored generated column and builds an index on the refusal table;
its initial backfill takes a table lock. Plan that schema replay using the existing
migration-owner deployment procedure. Historical intents retain their original metadata.

Legacy database adoption was subsequently removed from this PR. Startup requires the
current `postgres` administrator and configured database. It no longer discovers the
old `aimee` administrator, renames `aimee_shared`, transfers application objects, or
regrants existing tables and routines. Current credential refresh and fresh provisioning
remain, and the deployment gate now tests fresh startup and restart ACL preservation.
Fresh provisioning, repeated current-role provisioning, ownership/ACL preservation,
runtime writes, and credential rotation passed against local PostgreSQL 18 after this
removal. Shell syntax, test registration, and clean-checkout documentation checks passed.
The replacement Docker restart/TLS test is wired into CI; Docker was unavailable locally.
