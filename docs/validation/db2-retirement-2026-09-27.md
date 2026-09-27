# PostgreSQL-only database provider retirement

## Implementation

Both `aimee-server` and `aimee-kb` obtain production database connections from the
Go PostgreSQL provider. The DB2 process, descriptor, generated wire catalog,
unused Go implementations, native libpq driver, and native connection pool are
removed. Principal 29 is reserved; existing grants for it are archived by identity
without transferring permissions. PostgreSQL principal 28 serves both placements.

The generic host session contract preserves transaction state, exact integers,
NULL versus empty values, SQLSTATE, bytea, notifications and paged results. It
shares the PostgreSQL runtime pool, reserves capacity for ordinary SQL clients,
refuses overflow, expires idle capabilities and discards failed or abandoned
connections. Native daemons no longer link libpq.

KB owns knowledge algorithms and schema under `src/modules/kb/c`; PostgreSQL
owns driver and connection management. Domain `db2_*` C API names and immutable
SQL identifiers remain compatibility names. The six relocated SQL inputs are
byte-identical to their previous versions; no data-table renaming is performed.
Vault custody and the append-only audit sink remain separate resource owners.

Native startup, offline bootstrap, Vault operator status/rewrap and the isolated
WORM worker use the same provider implementation. Fixed runtime and migration
profiles accept no substitute credential, validate the same database/namespace,
and separate runtime from migration roles. Migration authority is restricted to
the bootstrap thread and closed before serving. Operator sessions retain their
restricted role, verified TLS and absolute deadline policy.

The audit consumer queues durable writes to its writer thread: a synchronous
PostgreSQL request from the bus pump would wait for its own reply. Regression
coverage exercises startup retry, flush and shutdown while the sink calls back
through the real bus.

## Validation

- All 75 repository lint checks passed. The 650-target native unit run on CT109
  identified one asynchronous audit assertion; its corrected regression passed
  locally and on CT109.
- Full Go suite passed. PostgreSQL and memory race suites passed with required
  real database fixtures; native client fixtures ran with ASan/UBSan.
- Native transport checks cover large schema batches, multi-frame result sets,
  exact typed values, rollback/reset, abandoned streams, expiry, notification,
  deadlines, reconnect, invalid authority and credential redaction.
- Native audit concurrency, audit durability, capture-gap, fidelity transport,
  KB HTTP, Vault operator and organization rewrap checks passed.
- PostgreSQL-only ownership, process inventory, native boundaries, retired-grant
  migration, packaging and independent export checks passed. CMake thin-client
  build and isolated generated-header export build passed.
- CT109 on `.253`: both published-0.4.5 stores passed all 12 upgrade checks:
  identity, retained canary, new write, and persistence after recreation for both
  roles. Native KB schema upgraded from version 21 to 44. Runtime readiness,
  graph evidence scope and legacy query eligibility checks passed. Native audit
  writes were observed in PostgreSQL; daemon linkage contains no libpq.
- Live KB authorization (40 checks) and mTLS scope enforcement (5 checks) passed
  with no skips against the PostgreSQL provider on CT109.
- Published 0.4.1 KB upgrade and rollback passed all 12 checks on CT109,
  including retained authority, new writes, recreation, byte-identical original
  cluster and rollback canaries. Exact historical PostgreSQL grants gain the
  session stage; recorded operator restrictions and other policy edits survive.
- Live PAM, authorization residual and identity-mint gates passed on CT109.
  Offline authorities also linked with GCC 13/LTO using the private transport.
- A 1 MiB sketch round-trip passed through the native provider with ASan/UBSan;
  the real-PostgreSQL native feature suite passed. Native session cells have an
  8 MiB limit inside a 16 MiB frame; ordinary SQL stage limits are unchanged.
- The full WORM worker PostgreSQL gate passed on CT109: isolated claim/ack
  privileges, refusal of producer/admin authority, crash retry without duplicate
  events, three delivered events and zero broken audit-chain links.

The final native candidate repeated all 12 upgrade checks after restricting
migration authority to the bootstrap thread. A matching database/home rollback
restored both published 0.4.5 identities and canaries.

The upgrade exercises caught and fixed retired grants, the old schema version,
a schema batch larger than 1 MiB, and audit-bus self-deadlock. Hosted CI closeout
is recorded on the pull request; this record does not claim deployment.

Production CT100 remains on its existing 0.4.5 native-bridge overlay. Release
promotion and rollout are separate from this code retirement.
