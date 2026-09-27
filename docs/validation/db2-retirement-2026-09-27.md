# DB2 retirement: PostgreSQL as the shared database provider

## Required end state

Both `aimee-server` and `aimee-kb` must obtain database connections, transactions,
and migration execution exclusively from the PostgreSQL module. Knowledge,
memory, management, and Vault policy stay with their domain owners. Removing or
renaming DB2 metadata alone does not achieve this: the legacy C code still opens
connections and links libpq into the hosts.

## First removal: fidelity audit evidence

The live `kb_handle_evidence_fidelity` reader now calls the host-only
`memory.runtime` operation `fidelity-read`. The Go memory owner reads existing
artifacts through the same PostgreSQL module contract used by both roles.
Report and attribution count use one statement snapshot. Exact turn IDs and
public response fields are preserved. A missing report is `not_evaluated`;
a failed owner/query is `evidence_unavailable`, never an invented empty report.

Removed `src/modules/db2/c/fidelity.c`, its header, and its SQLite-shim test.
Repository call-site inspection found the native writer APIs were called only
by that deleted test. The deferred judge remains disabled; no write API or schema
migration was added. The native transport now has its own test; Go tests use the
real PostgreSQL module store wire in an isolated disposable database for both roles.
The C-retirement guard prevents the removed files from returning, and the temporary
fidelity-specific source-boundary exception is removed.

## Remaining migration order

1. Retrieval evidence and attribution (`demotion.c`, `artifacts.c`,
   `evidence_lifecycle.c`): preserve typed source IDs, first-writer semantics,
   compare-and-swap merges, outcome history, and audit records in the memory owner.
2. Knowledge/code ingestion and indexing: move the remaining domain operations
   behind their owning module contracts while retaining scoped visibility.
3. Enrollment, management journals, tenant grants, and Vault database operations:
   preserve identity and authority separation; migrate runtime and offline tools.
4. Knowledge schema bootstrap and hardening: preserve existing schemas, migration
   checksums, role permissions, and upgrade/rollback behavior through PostgreSQL.
5. Delete the final C pool/driver (`db2_pool.c`, `db_postgres.c`), DB2 process
   contract, descriptor, and host linkage only after every live consumer is migrated.

The direct-libpq surface still includes `db_postgres.c`, `db2_hardening.c`, and
Vault operator status/rewrap runtimes. This change is the first deletion, not a
claim that DB2 or direct host database access has been eliminated.

## Validation

Focused Go race tests passed with real PostgreSQL-module execution for both roles:
retained reports, exact turn identity, attribution isolation, missing reports,
all four audit states, malformed payloads, database failures, invalid inputs,
and rejection of public RPC principals.

The native fidelity transport test passed. Source ownership, package ownership,
bus boundaries, module documentation, declaration ledger, source shrink-only,
and retirement checks passed. Full memory and PostgreSQL race suites passed
(361 seconds and 11 seconds respectively). Both native daemons compiled on CT109;
the actual linker probe and all 79 linkage-check tests passed with the release
Ubuntu/PostgreSQL toolchain.

CT109 retained published-0.4.5 Server and KB stores passed all 12 upgrade checks:
original identities and canaries survived, new writes succeeded, and recreated
containers retained the new writes. The old release read a seeded fidelity report
before upgrade; the candidate read the same report and attribution count through
the live audit route afterward. Missing evidence remained `not_evaluated` in both.

Both fixtures then rolled back to published 0.4.5, with their original canaries
retained. The rollback restored the complete database and matching home/Vault
snapshot; it did not attempt to overlay an older schema onto the upgraded store.

The DB2 closure still contains 114 native translation units. Production CT100 has
not been changed by this retirement work.
