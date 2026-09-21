# Shared conditional deletion and durable retry identity

The Go shared-memory owner now accepts `expected_version` on delete/forget and
an authenticated `idempotency_key` with that precondition. Existing behavior is
preserved: model authority retires eligible model-authored records; independently
verified user authority may physically delete through the existing deletion
contract. Body claims do not grant that authority. Model retirement continues to
respect episode/experience immutability and instruction/policy revocation rules.
This does not complete the broader configured retention/erasure policy.

The existing actor/owner/key receipt namespace serializes duplicates. Admission
locks the canonical row and checks scope, revision and authority before opening
an audit commit. The mutation, WORM changeset, invalidation and receipt commit
together. Destructive changesets are explicitly irreversible. A late receipt
failure rolls back the entire operation; an uncommitted disconnect leaves the
key available, while a committed retry returns the original commit.

Shared deletion receipts use schema version 2 and bind `target_version` plus an
`outcome` of `retired` or `destroyed`. Retirement includes its resulting `version`.
Destruction has no current canonical version, so that field is omitted. Existing
schema-one correction and private-retirement receipts retain their format and
hashes. No receipt caches memory content or certifies downstream consumers.

Schema 27 extends the existing shared receipt table; existing rows retain the
`correction` discriminator. A durable guard requires the exact target/result
revision and the canonical audit outcome. Go seals the changeset before inserting
the receipt. Replay checks the receipt's
current owner and scope and the actual canonical outcome. Its bounded privileged
verifier accepts only the caller's own key and returns a boolean: a restored ID
hidden by RLS cannot be mistaken for successful destruction. Changed requests
conflict; reactivation, scope/revision changes or erasure cannot cause another
retirement. Owner rotation and complete restore/retention semantics remain open.

Unkeyed calls retain their original path. Conditional calls add one locked
admission read; keyed calls add receipt lookup and verification. Lookup uses the
existing receipt primary key and canonical record ID. No latency improvement or
whole-request P95 claim is made here.

Local validation passes:

- Required PostgreSQL memory suite under the runtime role, including receipt
  failure rollback, stale/hidden targets, cross-verb reuse, authority refusal,
  exact schema-two outcomes, forged-receipt rejection, unkeyed preconditions,
  MCP replay audit suppression and hidden-ID restoration.
- Actual concurrent connections for both authority modes, committed response
  loss, uncommitted disconnection and replay after reconnect; targeted race tests.
- Full schema 26 → 27 → 27 upgrade on a disposable database using the previous
  revision's real schema, preserving every old correction-receipt field and the
  canonical record. Verifier ACLs remain restricted after reapplication.
  [Upgrade source identities and results](memory-shared-deletion-2026-09-21/schema-upgrade.json).
- Native build and memory-route tests, independent Go memory export/build, all
  77 repository lint checks and all 17 S1 contract checks.

CI exposed an error in the earlier standalone export repair: embedded SQL was
listed as event contracts. The descriptor now declares its 32 SQL inputs as
`go_assets`; contracts retain their JSON-only event-directory boundary. Asset
ownership rejects missing declarations, cross-module paths and symlinks, and the
export/build checks verify copied bytes and CMake dependencies. The standalone
Aimee export builds and its startup checks pass with this declaration. All 47
descriptor tests, all 77 lint gates and all 62 script regression files pass. The
CMake-only export test is skipped locally because CMake is unavailable; the Go
export/build regression executes successfully.

The first fresh image (`eec3d6d651`) passed all 573 standalone T3 checks. Enrolled
T2 passed 130 shared checks, including destructive HTTP commit/retry and failure
rollback, before its new model-retirement fixture lacked a project. Diagnosis
found a real admission defect: a contextless MCP store wrote the read-restriction
`__aimee_scope_missing__` marker as a project, then ordinary reads could not find
that row. The Go canonical writer now refuses the marker, including internal Put,
workflow and practice callers. The public owner reports `active_context_missing`;
explicit context/global writes retain their contracts. The updated live fixture
checks refusal leaves no row, then exercises retirement in an explicit project.
The [diagnostic receipt](memory-shared-deletion-2026-09-21/diagnostic-eec3d6d651.json)
records counts, image identities and cleanup of the nine owned containers and
nine empty networks. Images, volumes and raw receipts remain. This failed T2 run
is diagnostic evidence, not fresh shared-deletion acceptance.

Fresh application/harness `037dc8f8c38e4f737a65d53d8fcadc815d0e3421` passes
**1,472/1,472 checks**: **899 enrolled T2**, **573 standalone T3**. The 254 shared
checks include real HTTP/MCP deletion and retirement, failure injection, exact
receipts, KB restart, reactivation/erasure and hidden-ID restoration. The
missing-context refusal is checked through MCP before a scoped model retirement.
Both placements retain all 195 private-memory, 298 provider-boundary and 37
asynchronous native-worker checks.

- [T2 verdicts and native/provider receipts](memory-shared-deletion-2026-09-21/fresh-t2-037dc8f8c3.json)
- [T3 verdicts and native/provider receipts](memory-shared-deletion-2026-09-21/fresh-t3-037dc8f8c3.json)
- [Exact image identities](memory-shared-deletion-2026-09-21/image-identities-037dc8f8c3.json)

Application image: `sha256:362126872d9139ccd53f5670cf39da1ea63c2b5dfbdbe69fab8570975f4ccdc7`.
The pinned PostgreSQL and embedder images remain unchanged. All nine identities
and all three application containers' actual 32 KiB provider caps were verified.
Maximum native request sizes were 26,475 bytes in T2 and 25,980 bytes in T3.
These are correctness and size checks, not a new latency claim or completion of
token-budget and durable-dispatch acceptance.

An additional run of `check_module_docs.py` reports existing catalog/layout debt:
the historical memory checkpoint file is under `docs/modules`, and the memory
and benchmark guides have extra top-level sections. That catalog check is not
included in the passing 77 lint gates or deployment counts above.

After collection, all nine owned containers and nine empty owned networks were
removed. Images, volumes and raw receipts remain available on CT 9498.

Creation retries, remaining mutation preconditions,
consumer/checkpoint/retention behavior and the full MR-02 acceptance inventory
remain open. The C bus is unchanged; schema bootstrap remains with the existing
storage owner. Whole-DB2 retirement stays deferred.
