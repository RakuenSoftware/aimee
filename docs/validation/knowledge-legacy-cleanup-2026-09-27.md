# Knowledge storage legacy cleanup

This follow-up to the PostgreSQL provider retirement removes the obsolete DB2
namespace from the active application. Both `aimee-server` and `aimee-kb` use
the PostgreSQL module. Knowledge algorithms and schema remain in the KB owner;
they are not a second connection provider.

## Removed

- Native `db2_*` APIs, headers, source and adapter filenames, object paths,
  build targets, test environment variables and current operational terminology.
  The remaining domain APIs use `kb_store_*`; database sessions belong to
  `src/modules/postgres/client` and the Go PostgreSQL provider.
- The delegate planner’s obsolete schema-path rewriting and the code audit’s
  dynamically reconstructed legacy API marker. Missing old paths are reported
  directly; the audit recognizes the current knowledge-store API.
- The unused native evaluation-store implementation and its connection URL
  accessors, unused vector configuration accessors, and an unreachable setup
  component advertising the retired database setting.
- Legacy credential provisioning and Vault-to-runtime loading. An inherited
  `AIMEE_DB2_URL` is scrubbed without being provisioned. Current settings reject
  the retired namespace and filter old external config defaults from snapshots.
- Retired configuration documentation and operational script assignments,
  including a one-off updater for an already removed DB2 declaration ledger.
- Public health names: use `postgres_ok`, `knowledge_tables_ok` and
  `postgres_ready` where the corresponding old fields existed.

## Deliberately retained

| Reference | Why it remains necessary |
| --- | --- |
| Old credential spelling in secret classification, rejection and environment scrubbing | Old installations must not expose or accidentally reprovision a retired database password. This is not a runtime fallback. Existing sealed Vault records are not destructively erased. |
| `db2_done` as schema migration input | Schema 45 converts persisted in-flight erasure work to `knowledge_done`, preserving counts and replay semantics. Runtime never writes the old state. |
| Old health fields in the published-release upgrade probe | The published old binary cannot report newly named fields. Candidate probes use the new contract. |
| Old module paths, identifiers and symbols in regression guards | These checks must recognize and reject reintroduced legacy implementations, including mechanically renamed native memory APIs. |
| Published versioned SQL migrations | Their checksums include comments. Historical wording must remain byte-for-byte identical for existing installations to accept the migration ledger. The boundary guard pins these files. |
| Historical proposals, immutable native retirement inventory and frozen benchmark evidence | These describe past implementations and remain audit evidence. Current ownership mappings track the renamed paths without rewriting the original inventory. |
| Inert metadata in the pinned external config dependency | The application filters these defaults and rejects writes. This change does not manufacture a new upstream dependency release. |

Unrelated hexadecimal hashes and Unicode/CRC table constants containing the
characters “db2” are data, not legacy storage code.

## Upgrade and rollback contract

Schema 45 changes the erasure state constraint and maps the retired state before
installing the new constraint. `tests/e2e/knowledge-schema-upgrade.py` runs the
complete schema twice against seeded pending, in-flight and completed requests,
checks saved deletion counts, rejects the old state and exercises resumed and
completed requests through the real SQL function. Its transaction rolls back.
The required PostgreSQL CI job executes this regression.

Rollback uses the preserved original cluster/home from the documented upgrade
procedure. An older binary must not be pointed at a schema-45 database as an
in-place downgrade.

## Validation

- All 75 lint checks pass; the provider/namespace guard has 12 regression tests.
- Full Go tests, real PostgreSQL memory/evaluator race replay, schema-45
  migration/reapply/erasure replay, 235 frontend tests and TypeScript checks pass.
- All 634 script tests pass (one CMake test skipped because CMake is absent
  locally, then passed independently on CT109); all 19 semantic context checks
  and eight memory reliability checks pass.
- Native daemon builds pass. Native unit tests and standalone ASan/UBSan
  executables are exercised on CT109.
- CT109 on .253: both published-0.4.5 stores pass all 12 upgrade checks,
  including identity, retained rows, new writes and container recreation.
  Restoring the matching original database/home passes for both roles.
- The published 0.4.1 KB upgrade passes all 12 checks, including unchanged
  original cluster bytes and reads after rollback.

The first upgrade attempt caught comment-only checksum changes in two published
migrations; those files were restored exactly and the corrected upgrade passes.
Auxiliary sanitizer failures now fail the Make target immediately. Standalone
sanitizer executables use non-PIE linking to avoid the ASan shadow mapping
startup failure reproduced on CT109; address/undefined instrumentation stays
enabled, and production PIE flags are unchanged.

The rollback fixture now restores database ownership and ACLs as well as the
ordinary dump, matching the published image's initialization contract.
Production CT100 and its native bridge overlay are unchanged.
