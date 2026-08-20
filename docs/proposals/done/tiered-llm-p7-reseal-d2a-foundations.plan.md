# P7-reseal-d2a canonical receipts and typed verification foundations

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and validated on PostgreSQL 17 (CT260), with focused
  default and ASAN/UBSAN tests; production-uninvoked and disabled by construction.
- **Depends on:** P7-reseal-a/c and P7-reseal-d1.
- **Enables:** P7-reseal-d2b's injected DB x custody reconciler.

## Scope and delivery boundary

Provide the serialization, database, and verification foundations required by a
later crash-resumable whole-vault reconciler without introducing that reconciler
or any way to invoke reseal:

1. one canonical, versioned wire encoding for
   `vault_tpm2_reseal_receipt_t` and its SHA-256 digest;
2. a typed C wrapper around the existing owner-only P7 Postgres state machine;
3. owner-only, bounded, SERIALIZABLE post-promotion verification functions that
   bind live rows to the preserved staging plan; and
4. one byte-identical KEK-check sentinel implementation shared by jsonfile,
   Postgres, and D2b.

D2a adds no route, RPC, CLI, scheduler, startup recovery worker, background
thread, signal hook, config switch, or automatic operation. It does not generate
a KEK, acquire a maintenance guard, call custody/TPM, re-wrap or promote a DEK in
production, complete/quarantine an operation, clean prepared artifacts, drain
the WORM outbox, or authorize operational unseal. Tests call the new seams only
through unit or explicit integration binaries.

## Canonical receipt codec

Add a TPM-build-independent `modules/vault/vault_reseal_receipt.[ch]`. Native C
struct layout is never persisted, hashed, compared, or transmitted. Version 1 is
exactly 208 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | domain magic `AIMRSEAL` |
| 8 | 2 | version 1, network byte order |
| 10 | 2 | reserved flags, exactly zero |
| 12 | 4 | payload length, exactly 192 |
| 16 | 16 | operation ID |
| 32 | 8 | old generation, network byte order |
| 40 | 8 | new generation, network byte order |
| 48 | 32 | predecessor digest |
| 80 | 32 | capsule digest |
| 112 | 32 | future digest |
| 144 | 32 | new-KEK digest |
| 176 | 32 | manifest digest |

Expose fixed-size, allocation-free encode/decode/digest APIs. Encoding requires
non-null inputs, `old_generation <= INT64_MAX-1`, and
`new_generation == old_generation + 1`. Decoding zeroes output before parsing and
requires exact length, magic, version, reserved bits, payload length, generation
relationship, and no trailing representation. Unknown versions fail closed.
Inputs and outputs may not overlap. Every failure zeroes receipt/digest outputs
and cleanses temporaries. The digest API first validates the supplied bytes with
the strict decoder, so it cannot bless a noncanonical version-1 buffer. Receipt
equality is constant-time equality of two already-decoded-and-re-encoded canonical
buffers; the database digest is SHA-256 of exactly those bytes. Operation IDs map
injectively between the receipt and SQL as exactly 32 lowercase hexadecimal
characters (two characters per byte, no prefix, separators, uppercase, or
alternate UUID spelling). The existing TPM manifest format is unchanged.

## Typed Postgres wrapper

Add `db2/org_vault_rewrap.[ch]` as D2b's sole SQL surface. It owns query text,
binding, row decoding, length/range checks, transactions, result classification,
and output cleansing. D2b may not name tables, call `aimee_pg_*`, inspect database
error prose, or read rewrap tables directly.

Use closed state and result enums. Unknown/null states, unexpected row counts,
negative/overflowed counts, malformed operation IDs, wrong receipt/wrap/digest
lengths, and null required columns are integrity failures. A typed snapshot holds
the operation ID, state, seal epoch, fence, G/G+1, optional canonical receipt and
digest, counts/digests, failure class, and optional failure provenance. Whenever
a receipt exists, locally decode it and require its hash, operation ID, and
generations to match the row.

Retain the compatibility status function and add owner-only
`org_vault_rewrap_snapshot(TEXT)` returning the complete receipt plus every state
field in one MVCC statement. Never reconstruct a receipt from columns or perform
an unbound second table read.

The wrapper covers begin, snapshot, record-prepared, bounded source pages,
stage-DEK/check, stage-finish, mark-committing, mark-resealed, promote, complete,
abort, and recovery-required. Its explicit, connection/thread-bound handles own:

- one SERIALIZABLE staging transaction spanning source pages, staging writes,
  and stage-finish;
- one SERIALIZABLE promotion transaction containing promotion only; and
- one SERIALIZABLE verification/completion transaction spanning summary, all
  verification pages, guarded in-process cryptographic verification, and the
  exact completion call. It is read-only until the final completion transition.

No handle commits after an error and no transaction spans a custody call. Local
AES-KW verification under an already-guarded KEK is permitted inside the final
transaction; TPM/provider operations are not. Failed or abandoned handles attempt
rollback and reject reuse. A rollback failure poisons and discards the underlying
connection. Commit uncertainty, including failure from the COMMIT execution
itself, is a typed transient result and requires a new durable snapshot before
another edge.

Extend the PG adapter only as needed to retain five-character SQLSTATE from the
last prepare, step, or exec/commit failure; do not retain detail, hint, object
names, or other server prose in the typed layer. Classification uses SQLSTATE,
never message substrings:
explicit busy, conflict/reconciliation, invalid-contract, transient connection or
serialization uncertainty, integrity, and generic error remain distinct. Error
text and secret-bearing values are never logged.

All row/page outputs are caller-owned and bounded. Page limits are 1..128.
Secret cursors advance strictly by source ID. Check cursors are
`convert_to(principal,'UTF8')` byte strings ordered by PostgreSQL `bytea` ordering:
zero-length means start, each returned principal is nonempty and unique by the
existing primary key, the returned cursor must be strictly greater than the input,
and an empty page means exhausted. Duplicate, regressing, or non-terminating
cursors fail as integrity errors. Explicit destroy helpers
cleanse receipt, wrap, check, and digest buffers.

## Post-promotion verification SQL

Add owner-only SECURITY DEFINER functions with every object, function, operator,
and cast explicitly schema-qualified and `search_path=pg_catalog` only, exact
input validation, existing lock
order, exact operation/fence matching, `state='promoted'`, active sealed
`tpm2-reseal` maintenance identity, and mandatory SERIALIZABLE isolation:

```sql
org_vault_rewrap_verify_summary(TEXT, BIGINT)
  -> (secret_count BIGINT, check_count BIGINT,
      receipt_digest BYTEA, inventory_digest BYTEA, stage_digest BYTEA)

org_vault_rewrap_verify_secret_page(TEXT, BIGINT, BIGINT, INTEGER)
  -> (source_id BIGINT, principal TEXT, agent TEXT, cred TEXT,
      version BIGINT, wrapped_dek BYTEA)

org_vault_rewrap_verify_check_page(TEXT, BIGINT, BYTEA, INTEGER)
  -> (principal TEXT, kek_check BYTEA, principal_cursor BYTEA)
```

The byte cursor follows the exact `convert_to(...,'UTF8')` contract above and
avoids database locale/collation ambiguity. Summary is the transaction's global
set-equivalence gate. It requires
operation, staging, and live counts to match; bijective staged/live identities;
byte-identical promoted wraps/checks; fixed lengths; recomputation of the P7-c
canonical stage digest; and an exact deterministic `resealed` outbox checkpoint
bound to actor, consumed fence, receipt/inventory/stage digests, and state.

The original inventory digest is not recomputed from promoted wraps: those bytes
were intentionally replaced. It remains bound by the immutable operation,
preserved stage source digests included in the recomputed stage digest, and exact
resealed checkpoint.

Each page independently rechecks operation, fence, promoted state, sealed
maintenance identity, and SERIALIZABLE isolation. The typed wrapper additionally
permits pages only after summary on the same transaction handle, and permits
completion only after exactly the reported number of rows has been consumed and
cryptographically verified. The transaction holds the exclusive advisory xact
lock plus control/operation row locks through completion, so ordinary admissions,
mutations, and a competing terminal transition cannot serialize between the
verified snapshot and completion. Pages traverse the joined live/staged set deterministically
and return promoted live values for D2b cryptographic verification, never old
wraps. Secret wraps are exactly 40 bytes. Checks are exactly zero or 40 bytes;
empty checks remain represented and counted. The wrapper must consume exactly
the summary totals before verification can succeed. Any mutation, missing/extra
row, digest mismatch, stale fence, state mismatch, or isolation mismatch fails
closed. These functions never mutate, complete, quarantine, or clean staging.

## KEK-check consolidation

Move the private fixed-length 32-byte sentinel and its operations from `vault_store.c` and
`db2/vault_pg.c` into internal `modules/vault/vault_kek_check.[ch]` fixed-size
wrap and verify helpers. Their fixed-size signatures admit no length mismatch.
Wrap uses the existing AES-KW primitive. Verify unwraps
to a fixed local buffer, compares with `CRYPTO_memcmp`, and cleanses plaintext on
every exit. Null inputs fail; wrap output is cleansed on failure. The caller-owned
input KEK is never mutated and every helper-owned key-schedule/intermediate is
destroyed through the existing crypto primitive's cleanup contract. No file format,
Postgres format, derivation, first-unlock, or rekey behavior changes.

## Access control and D2b seam

All new functions stay owner/migration-orchestrator-only. Function creation and
the adjacent exact-overload revokes occur in the same schema transaction, closing
the default-PUBLIC window. Revoke exact overloads from PUBLIC and
`aimee_kb_runtime` beside definitions and in grant reapplication;
verify effective inherited privileges. Runtime keeps no direct table access.
The schema/grant test also enumerates every overload under each new function name
and rejects any unlisted executable signature. The staged/resealed WORM row is
protected by the existing UPDATE/DELETE/TRUNCATE denial triggers and is retained;
D2a/D2b have no drain or cleanup path.

D2a adds no invocation credential. D2b must use a dedicated least-privilege
orchestration connection restricted to exact functions, with no table access,
ownership, DDL, RLS bypass, or owner-role assumption. SQLite gains no authoritative
behavior; any necessary shim returns unsupported.

D2a freezes canonical receipt bytes, the typed SQL-only interface, explicit
transaction handles, bounded verification pages, the shared KEK-check helper,
and a const injected DB vtable. D2b owns reconciliation policy, its protected new
KEK arena, custody adapter, maintenance-guard choreography, retry loop, and the
completion/quarantine decision table.

## Validation gates

- Receipt unit/fuzz: pinned 208-byte known-answer/hash, boundaries through
  `INT64_MAX`, every header/length/generation mutation, every truncation/trailing
  byte, field inequality, zero-on-failure, arbitrary input under ASAN/UBSAN, and
  identical default/TPM2 behavior.
- KEK-check: compatibility with existing jsonfile/Postgres fixtures, correct and
  wrong KEKs, every-byte tamper, cleanse assertions, and unchanged unlock/rekey
  behavior.
- Wrapper unit: every state/evidence shape; malformed/null/oversized fields;
  count overflow; receipt/hash/op/generation mismatch; row-count anomalies;
  cursor regression; page caps; SQLSTATE mapping; commit uncertainty; rollback;
  handle misuse; binary-output cleansing and log scans.
- Real PG17: fresh/reapplied schema; full snapshots; required isolation/state/
  maintenance/fence checks; more than 512 secrets/checks including empty checks
  and byte-order edge principals; exact set acceptance; mutation of every live,
  stage, digest, count, and outbox dimension fails closed; disconnect and
  serialization outcomes; concurrent maintenance/control mutation and competing
  completion/quarantine are blocked or lose deterministically; PUBLIC/runtime/
  inherited ACL denial; exact overload enumeration; existing P1/P7
  gates remain green.
- Builds: default server, TPM2 server, focused tests, ASAN/UBSAN, and fuzz. Regenerate
  but do not track `schema_data.h`. Source/link inventory proves no production
  entrypoint reaches the new operation foundations. Scan logs/durable state for
  raw key material or newly exposed database payloads.

## Completion criteria

D2a completes when canonical receipt bytes have one strict implementation, the
duplicated KEK-check code is removed without behavior change, all P7 transitions
and promoted reads are available only through typed cleansing wrappers, PG can
prove promoted live/stage equivalence and page all rows within one SERIALIZABLE
snapshot, ACL reapplication stays closed, and no production invocation exists.

## Delivered validation

- Default server and kb builds plus focused receipt, KEK-check, typed-wrapper,
  pool, vault-store, TPM-stub, and prepare-classification tests passed.
- Receipt and vault-store paths passed ASAN/UBSAN, including alias failures,
  cleansing, canonical receipt binding, transaction phase misuse, response-ID
  mismatch, cursor termination, and commit uncertainty.
- The live typed wrapper test passed against PostgreSQL 17 on CT260 with a
  canonical 208-byte receipt. Fresh and reapplied schema/grants, exact ACLs,
  verification evidence, concurrency/failure cases, and the complete P1/P7
  PostgreSQL regression gate passed.
- Successive adversarial full-branch roundtables were audited claim-by-claim.
  Their recurring valid findings were incorporated; the release-gate review
  reported no surviving issues on the hardened tree.
