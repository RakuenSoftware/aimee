# Proposal: a `privacy` module: data-subject rights over everything Aimee persists

- **State:** PENDING. Module specification and phasing; no code in this PR. The
  descriptor and canonical-doc outline in §10 are the contract an implementation
  must hit.
- **Author:** JBailes
- **Date:** 2026-08-27
- **Charter roles:** Classify-Score (subject-data registry), Plan (dry-run
  report), Enforce (erasure, restriction, requester authority), Attest (signed
  receipts), Constrain-Verify (coverage latch, residual verification)
- **Proposed module id:** `privacy` (optional, `enabled_by_default: false`)
- **Depends on:**
  [`governed-recall-decision-record-and-adversarial-memory-measurement.md`](governed-recall-decision-record-and-adversarial-memory-measurement.md)
  §6 (erasure as durable state). Erasure without tombstones is undone by the
  next backup restore, so P3 below cannot ship before it.
- **Related:** [`proposal-evidence-provenance-tiers.md`](proposal-evidence-provenance-tiers.md)
  (provenance classification at write time).

---

## Thesis

The request workflow is the easy part. Intake, plan, approve, execute, verify is
five endpoints and a state machine, and any competent implementation gets it
right in a week.

The hard part, and the only part worth designing carefully, is this:

> **Which of the things Aimee persists contain data about a person, and where
> does the derived copy live?**

Aimee persists a lot. `src/modules/db2/c/schema.sql` declares **241 tables**
(the SQLite mirror declares 237). DB1 is a separate store behind its own module
boundary, and its table count **could not be established by inspection at all**:
there is no single schema file to count, which is itself an instance of the
problem rather than an aside. On top of both sit pgvector points, on-disk
session and trajectory artifacts, the audit log, KB documents, and a long tail
of *derived* objects: `memory_chunks`,
`memory_units`, `memory_summaries`, `memory_episodes`, `memory_aliases`,
`memory_entities`, `memory_event_frames`, `entity_profiles`, `entity_edges`,
`memory_lineage`, and the embeddings computed from all of them. A subject's
sentence enters once and lands in a dozen places under a dozen ids.

That produces the failure mode this module exists to prevent:

**A hand-written list of tables is wrong the day someone adds the next one.**
This is not hypothetical: DB2 grew by twenty-one tables between the branch this
work started on and the branch it merges into. An
erasure implemented as "delete from these seventeen tables" degrades silently:
it keeps returning success while a new derived store quietly accumulates copies
nobody enumerated. The absence of an error is not evidence of erasure.

So the load-bearing idea is not the workflow. It is a **declared, CI-enforced
disposition for every persisted store, defaulting fail-closed**, on exactly the
pattern the tree already uses twice: `rel_types.sensitivity` is `NOT NULL
DEFAULT 'pii'` so an unclassified relation is withheld rather than leaked, and
`ownership_complete: true` makes an undeclared module-local source a CI failure
rather than a silent gap. This proposal applies the same latch to persistence.

Everything else here is downstream of that registry.

---

## §0 What already exists (DRY map)

Verified against `a397223849`.

| Piece | Existing surface | State |
| --- | --- | --- |
| Per-memory removal, split by authority | `memory_delete_as` (`memory_core_crud.c:621`): MODEL authority retires (row survives as `key#vN`, `valid_until` stamped, readable via `memory_fact_history`); USER authority hard-deletes through `memory_delete` (`:632`), which wipes `memory_provenance`, drops the record's own and every unit-scoped pgvector point, and deletes the row | **exists**: the right primitive, at the wrong granularity for a subject |
| Destructive-edit gate | `memory_authority_t` (`src/headers/memory_authority.h`) plus `CAP_MEMORY_ADMIN` (`headers/server.h:156`) | **exists**: a model cannot destroy what a user stated |
| Cascade fan-out | **23** `REFERENCES memories(id) ON DELETE CASCADE` children in `src/modules/db2/c/schema.sql`: chunks, units, aliases, entities, episodes, relations, summaries, temporal refs, event frames, scopes, workspaces, links, conflicts and more | **exists**: derived rows follow the parent, **if** the parent is found |
| Scope columns | `memories.scope_type` / `memories.scope_value` (default `global` / `_global`), plus `memory_workspaces`, `memory_scopes`, `memory_entities`, `entity_profiles.entity_id` | **exists**: candidate subject-key paths |
| Persisted write provenance | `memories.provenance_category`, fail-closed to `agent_message` (`schema.sql:605`), written from the calling surface's authority via `MEMORY_PROVENANCE_FOR` (`memory_core_crud.c:493`) | **exists and is populated** |
| Lineage | `memory_lineage(object_type, object_id, source_kind, source_ref, …)` | **exists**: how a record entered, per assertion |
| Fail-closed sensitivity default | `rel_types.sensitivity TEXT NOT NULL DEFAULT 'pii'` (`schema.sql:2052`); `memory_pii_rel_sensitivity` withholds unknown types | **exists**: the exact pattern §2 copies |
| Withholding at the shared boundary | `FACT_GATE_REJECT_SENSITIVE` (`src/modules/memory/memory_fact_gate.h`): credential/regulated-PII relations stay local and never reach the shared KB | **exists**: an existing personal-data boundary |
| Tamper-evident evidence | `audit_worm_append` (`src/modules/audit/audit_worm.c:135`), `_verify` / `_checkpoint` / `_seal` / `_read_page`; keys under vault custody (`docs/modules/audit.md`) | **exists**: the receipt store, already built |
| Non-content audit contract | The memory audit hook is documented as taking **non-content fields only** (`headers/memory.h:428`) | **exists**: the property §5 depends on |
| Hash-chained KB events | `kb_audit_event(actor_role, actor_principal, action, subject, verdict, prev_hash, row_hash)` (`schema.sql:183`) | **exists**: already carries an `actor_principal` |
| Action authorization | `policy_check_tool` (`src/server/execution_policy_bus.c:26`, declared `headers/agent_exec.h:236`) | **exists**: where an execute must be gated |
| Organizational identity | `governance` module: OIDC issuer profiles, org accounts/roles, tenant binding (`docs/modules/governance.md`) | **exists**, optional; supplies a verified requester when present |
| Signing custody | `vault`: signing material and protected references (`docs/modules/vault.md`) | **exists**: receipt keys have a home |
| Scoped identity | `workspace`: scoped resource identity, containment, `ws_scope_*` (`docs/modules/workspace.md`) | **exists**: the containment boundary a subject scope rides on |

### The verified gaps

- **No subject concept anywhere.** There is no data-subject identifier, no
  registry of which stores hold subject data, and no query that answers "what do
  we hold about this person". `memories.scope_value`, `memory_entities.entity`
  and `entity_profiles.entity_id` are the nearest things and none is a declared
  subject key.
- **No store-level classification.** Nothing marks a table as containing,
  deriving, or excluding subject data, so nothing can fail CI when the next
  store arrives unclassified. Note the contrast: the *row-level* fail-closed
  default already exists (`rel_types.sensitivity`), and the *store-level* one
  does not.
- **Erasure is per-id, and leaves no state.** `memory_delete_as` takes an
  `int64_t id`. There is no subject-scoped erasure, and the destructive branch
  leaves nothing behind, so a restore resurrects erased content and a re-ingest
  stores it fresh. The retire branch is not erasure: it deliberately keeps the
  content readable.
- **Restore is now entirely outside Aimee.** The `aimee data db`
  backup/check/recover group was removed when the store became PostgreSQL
  (`cmd_data.c:534`): backups are `pg_dump`'s job. The process has no hook at
  which to notice that a restore just undid an erasure.
- **Derived stores are only reachable through their parent.** The 23 cascades
  are correct and are not enough: subject data that entered through a path with
  no `memories` parent row (KB documents, interaction-event embeddings, entity
  profiles, trajectory artifacts on disk) is not reached by deleting memories.
- **No export.** Nothing produces a machine-readable bundle of what is held
  about one subject.
- **No receipt.** `mem_audit("memory.delete", …)` records that a row was
  deleted, by id and without content. Nothing records what a *request* targeted,
  what each store confirmed, and what could not be confirmed.

---

## Part II: The module

### §1 Naming and boundary

The module is `privacy`, not `dsar`. DSAR is the *request surface*; the module
owns more than the request: the subject-data registry, subject resolution,
export, erasure, restriction, retention dispositions, and receipts. A request
workflow is one consumer of those. Naming it `dsar` would make the registry look
like an implementation detail of a workflow, when the dependency runs the other
way and the registry is the part that must outlive any particular intake
channel.

`privacy` **owns**: the subject-data registry and its latch, subject resolution,
the rights operations (access, portability, erasure, restriction,
rectification), the request lifecycle, and signed receipts.

`privacy` **does not own**: the audit ledger (it appends to `audit`), the
decision to authorize an action (it asks `execution-policy`), signing material
(`vault`), organizational identity (`governance`), memory semantics (`memory`),
or any intake connector (out of core, see §9).

### §2 The subject-data registry, and the latch that keeps it honest

A declared disposition for every persisted store: each DB2 and DB1 table, each
pgvector namespace, each on-disk artifact class.

| Disposition | Meaning | Required fields |
| --- | --- | --- |
| `subject_data` | Holds data about a person directly | the subject-key path: how to go from a subject id to the rows |
| `derived` | Holds material computed from another store | the parent store, and whether the parent's cascade actually reaches it |
| `no_subject_data` | Structural, operational, or code-derived only | a one-line justification |
| `evidence_retained` | Deliberately not erased (see §5) | the retention rationale |

Three properties make this a control rather than a document:

1. **Fail closed.** An unclassified store is treated as `subject_data` with an
   unknown key path, which makes every request involving it *incomplete*. Not
   "assume clean and proceed": a request that cannot enumerate a store reports
   that it could not, and the reported outcome is partial.
2. **CI-enforced coverage.** A `check-subject-data-coverage` latch parses
   `src/db2/schema.sql` and `src/db1/schema.sql`, and fails when a `CREATE TABLE`
   has no registry entry. This is the same shape as the existing
   `ownership_complete` rule, and it is the one mechanism that keeps the module
   correct as the schema grows.
3. **`derived` entries assert their reachability.** A `derived` entry that
   claims the parent cascade reaches it is a testable claim, and §8 tests it by
   erasing a parent and asserting the child is gone. The cascades in
   `db2/schema.sql` are real, and the registry should prove it rather than trust it.

The registry is also the honest answer to "can you delete my data?" before any
request arrives: it is a map of exposure, readable on its own.

### §3 Subject resolution

A subject is a declared identity with a scope, resolved to a candidate row set
across the registry. Two things matter more than the mechanism:

**Resolution is a first-class, reviewable step, not a join.** Aimee holds
free-text memories about people who are not users. A subject scope built from an
exact key (`memory_scopes`, `memory_entities`, an account id) is precise and
incomplete; one built from name matching is broader and wrong in both
directions. The plan report (§4) shows *how* each candidate was matched and at
what confidence, so an approver can see the difference between "this row is
keyed to the subject" and "this row mentions a common first name".

**A joint record belongs to more than one subject.** "The user said their
colleague is on leave until March" is data about two people. Erasure and export
must treat those differently: erasing on behalf of the colleague should not
silently destroy the user's record of their own statement, and exporting to the
user should not disclose the colleague. The registry marks stores that can hold
joint records; the plan flags them individually; and the default for a joint
record under erasure is **redact the subject's portion, retain the rest**, with
the decision visible in the plan rather than buried in a policy default.

### §4 The request lifecycle

Four operations, each separately auditable, each with a contract that is more
than a name.

**`plan`** is a **dry run that touches nothing**, and this is a hard contract,
not a convention: it opens no write transaction. It produces the report an
approver reads, listing per store the matched rows, the match basis and
confidence, joint records, derived material that will follow, and anything the
registry could not enumerate. A plan that reports "0 stores unenumerable" is the
only kind that should be approved without discussion.

**`approve` / `reject`** record a human decision against the plan, with the
approver's identity and the plan's hash. Approving a *stale* plan is refused:
if the data changed since the plan was produced, the approval is for a report
that no longer describes reality.

**`execute`** is **idempotent per request id**. A retried, duplicated, or
replayed execute performs the work exactly once and returns the original
outcome. This is the property that makes the operation safe to expose to any
retrying caller, and it is cheap to get right at the start and expensive to
retrofit.

**`verify`** returns **evidence, not an acknowledgement**. It re-runs the
enumeration after the fact and reports what it now finds: zero residuals, or a
count and a location. A verify that always returns "ok" is worse than no verify,
because it converts an unknown into a false assurance. Residuals are a normal
outcome (a replica lagging, an unenumerable store, a joint record retained by
design) and must be reportable as such.

### §5 Erasure, and the audit trail that must survive it

Erasure composes three things that already exist or are already proposed:

1. **Physical removal** via the existing primitives: the row, its 23 cascades,
   its pgvector points. The destructive branch of `memory_delete_as`
   (`memory_core_crud.c:632`) already does exactly this per memory and is the
   right building block. Subject-scoped erasure is USER authority by
   construction, so it takes that branch and must clear `CAP_MEMORY_ADMIN`.
2. **A durable tombstone** outside the memory graph, from the recall proposal's
   §6, so a backup restore cannot resurrect the content and a re-ingest is
   quarantined rather than stored fresh. **This module cannot honestly claim
   erasure without it**, which is why P3 is sequenced behind it.
3. **A receipt** (§6).

**The audit ledger is deliberately not erased, and this needs stating plainly
rather than resolving quietly.** The WORM ledger is the proof that the erasure
happened; erasing it destroys the evidence the subject's own request generated.
The resolution is structural, not an exemption: the ledger records *decisions,
identifiers and hashes, never subject content*, so there is nothing in it to
erase. That is a claim the registry must back, with `audit`-owned stores
classified `evidence_retained` and a test asserting that no subject content
reaches an appended record. The memory audit hook already documents itself as
non-content (`headers/memory.h:428`), so the claim starts from a stated contract
rather than from hope, and the test pins it. If that test cannot be made to pass, the ledger
schema is wrong and should change, rather than the claim being softened.

**Restriction (Art. 18) is not erasure and should not be implemented as one.**
Restriction means retained but not used, which is exactly a recall-time
exclusion reason. It composes with the recall decision record rather than
touching storage at all, and it is the cheaper, safer, and more common
operation. Where a request is ambiguous or the subject's identity is unverified,
restriction is the correct provisional action while the request is reviewed.

### §6 Receipts

A receipt records what a request targeted, what each store confirmed removed,
what could not be confirmed and why, the requester and approver identities, the
timestamps, and the ledger head hash at completion. Signed with a vault-held key
and appended to the WORM ledger.

Its honesty properties are the point. A receipt that reports a residual is doing
its job; one that reports clean success for a store the registry could not
enumerate is manufacturing false evidence. **The receipt is evidence of the
action the module took. It is not an attestation that no copy of the data exists
anywhere**, and its own wording should say so, so that the artifact cannot be
quoted as more than it is once it is detached from this document.

### §7 Requester authority: the request is untrusted input

An erasure request is a destructive, hard-to-reverse action arriving over a
channel. It is exactly the shape of input this codebase already treats as
untrusted everywhere else, and it deserves the same treatment.

- **Strict mode holds rather than executes.** An unverified, unauthenticated, or
  anomalous request (a bulk erasure, a subject scope resolving to an implausible
  fraction of the store) is queued for human approval, not run.
- **`execute` is not an agent-callable tool by default.** An agent may `plan`,
  which is read-only and useful. Exposing `execute` to a model that reads
  untrusted content puts subject-data destruction one indirect prompt injection
  away. If it is ever exposed, it is behind the same approval gate a human
  request goes through, and `policy_check_tool` is the enforcement point.
- **Identity comes from the transport, not the payload.** A request that names
  its own requester is asserting, not proving. Where `governance` is enabled, the
  verified principal is available; where it is not, the module runs in an
  operator-local mode and says so in the receipt rather than implying an
  authentication it did not perform.

### §8 Tests and failure behavior

The module's failure mode must be **incomplete, loudly**, never **complete,
quietly**. Concretely:

- An unclassified store makes every overlapping request partial, and the
  partiality appears in the plan, the receipt and the verify.
- A `derived` registry entry claiming parent-cascade reachability is tested by
  erasing a parent and asserting the child rows and vector points are gone. This
  is the test that catches a new derived table added without a cascade.
- A restore-then-verify test: erase, back up, restore an *older* snapshot,
  re-verify. The erased content must not be servable. Without the tombstone this
  test fails, which is the intended signal, not a flaky test.
- A re-ingest test: erase a value, ingest the same text again, assert it is
  quarantined rather than stored clean.
- An idempotency test: execute the same request id concurrently and after a
  crash; assert exactly-once.
- A joint-record test: erasure for subject A leaves B's record intact; export for
  A does not disclose B.
- A ledger-content test: no appended audit record contains subject content.

### §9 What this module is not

- **Not an ITSM or ticketing integration.** Intake connectors (email, queue,
  webhook, drop folder) are adapters over the four operations and belong outside
  core. The module must be complete and testable with no connector at all.
- **Not consent management.** Consent capture is an upstream concern; this
  module enforces the consequences.
- **Not a compliance certification, and not legal advice.** It helps an operator
  meet obligations. Compliance is a property of the operator's whole process.
- **Not a promise of physical deletion everywhere.** Backups, replicas, and
  systems Aimee does not control are reported, not claimed.
- **Not able to recall what was already sent upstream.** This limit is specific
  to Aimee and belongs in the module's own documentation: memory content that was
  injected into a prompt and sent to a model provider has left the boundary. The
  module can stop it being sent again; it cannot unsend it. Any deployment claim
  that ignores this is false, and the receipt should not be phrased in a way that
  invites the reading.
- **Not a second audit store, not a second policy engine.** It appends to
  `audit` and asks `execution-policy`.

### §10 Descriptor and canonical documentation

Proposed `src/modules/privacy/module.yaml`:

```json
{
  "descriptor_version": 1,
  "id": "privacy",
  "dependencies": [
    "audit",
    "config",
    "execution-policy",
    "ir",
    "memory",
    "module-runtime",
    "protocols",
    "vault",
    "workspace"
  ],
  "enabled_by_default": false,
  "runtime_toggle": { "supported": false },
  "ownership_complete": true
}
```

Two descriptor choices worth defending:

**`runtime_toggle.supported: false`, and `enabled_by_default: false`.** Selected
before startup, and once selected it cannot be dropped at runtime. Toggling it
off while restrictions and tombstones exist would silently stop honouring them,
which is the one failure this module cannot be allowed to have. Decommissioning
is an audited operation, not a config flip.

**`governance` is not a dependency.** The module consumes a verified principal
when governance is present and runs operator-local when it is not, exactly as
`execution-policy` remains the local enforcement boundary when governance is
absent. Depending on it would make data-subject rights unavailable to
single-operator deployments, which inverts the intent.

`docs/modules/privacy.md` follows the enforced thirteen-section contract:
Purpose and non-goals / Public contracts / Dependencies and consumers /
Providers and readiness / Configuration and activation / Surfaces / Data and
migrations / Security and privacy / Supported journeys / Tests and failure
behavior / Operational diagnostics / Compatibility / Extension and removal.
`Providers and readiness` carries the load: readiness must separate registry
coverage, subject-resolution availability, tombstone-store availability, ledger
appendability, and signing-key custody. A module that can plan but cannot
tombstone is **not** ready for erasure, and must report that rather than a
single green light.

Proposed surfaces: an `aimee privacy` command family (`map`, `subject`, `plan`,
`approve`, `execute`, `verify`, `receipt`), matching `/v1/privacy/*` routes, and
a **plan-only** tool exposure per §7. No name collides with an existing command.

---

## Phasing

Each phase is independently useful and independently shippable, and the ordering
is a dependency chain, not a preference.

| Phase | Content | Gate |
| --- | --- | --- |
| **P1** | Subject-data registry + `check-subject-data-coverage` latch | Ships alone. Delivers the exposure map with no behavior change, and is the prerequisite for every later phase. |
| **P2** | Subject resolution + `plan` (read-only) | Ships alone. Non-destructive by contract, so it is safe well before erasure exists. |
| **P3** | `execute` erasure + receipts + `verify` | **Blocked on** the recall proposal's §6 tombstones. |
| **P4** | Export / portability bundle | Blocked on the joint-record decision (open question 2). |
| **P5** | Restriction and rectification | Blocked on the recall decision record (that proposal's §1), since restriction *is* an exclusion reason. |
| **P6** | Intake connectors | Out of core. |

P1 and P2 together answer "what do you hold about me, and what would deleting it
touch?" with no destructive capability in the tree at all. That is most of the
value and none of the risk, and it should not wait for P3.

## Open questions

1. **What is a subject identifier here?** An account, an OIDC subject claim, an
   `entity_profiles.entity_id`, or a request-scoped declared identity resolved
   at plan time? The last is the most honest and the least convenient, and it
   makes every plan a review step rather than a lookup.
2. **Joint records under export.** Redaction of the other subject's portion is
   the obvious answer and is hard to do well on free text. Is a conservative
   "exclude the whole record and list it as excluded" better than a redaction
   that might leak? It is worse for the subject and safer for the third party.
3. **Does the registry live in the schema or beside it?** A comment convention
   parsed out of `schema.sql` keeps the classification adjacent to the
   definition, which is where it will actually be maintained; a separate file is
   easier to validate and easier to let drift.
4. **Scope of the on-disk artifact classes.** Session transcripts, trajectory
   exports and workspace working trees hold subject content and have no schema
   to latch against. What is the enumeration mechanism, and is it in P1 or a
   phase of its own?
5. **Retention dispositions.** Does this module own default retention windows
   per store, or only report what other modules declare? Owning them centralizes
   a policy that several modules currently imply and none states.
6. **Verify cost.** A full re-enumeration across every registered store may be
   expensive on a large deployment. Is verify sampled with a stated confidence,
   or exhaustive and slow? A sampled verify must never report as an exhaustive
   one.
