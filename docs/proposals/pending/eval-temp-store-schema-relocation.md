# Retire the legacy eval temp store behind a Go-owned disposable-run contract

- **State:** PENDING — restored after rejection audit on 2026-08-15.
- **Corrective history:** PR #2660 rejected this live objective because the archived plan described
  a C/SQL scratch-store repair. That was an ownership error, not evidence that benchmark isolation
  was obsolete. This rewrite chooses the proposal's retirement branch, assigns benchmark-run
  isolation to Go, and leaves production memory schema and storage semantics with their existing
  owner.
- **Related residual:**
  [dataset-benchmark-direct-track.md](../rejected/dataset-benchmark-direct-track.md) separately owns
  the Go LoCoMo/LongMemEval runner and structured results. This proposal owns only disposable-run
  isolation and retirement of the broken scratch-store API.

## Decision

Retire `mem_eval_open_temp_db`, `db2_eval_open_temp_store`, and the private-schema bootstrap rather
than making the production DB2 schema relocatable.

`server-go/modules/benchmarks` will own a typed disposable benchmark-run lifecycle. It will obtain
an explicitly provisioned, single-use database lease, launch or address an isolated memory service
bound to that database, run cases only through memory ingest/retrieval contracts, and clean up only
the resource named by the lease. The benchmark module will not apply DB2 schema, issue benchmark
queries against DB2 directly, or become a second memory-storage owner. Normal memory startup remains
responsible for applying its production schema into `public` inside the disposable database.

The legacy C harnesses and test that call the scratch-store API must migrate to that Go lifecycle or
be deleted. Only after every caller is gone may the C scratch-store API and its shadow-schema logic
be removed.

This decision was approved by Aimee review `roundtable-c233450b73af5a25a58d8d4c`.

## Why the current store cannot be retained

In a libpq build, `db2_eval_open_temp_store_pg` creates a private
`aimee_eval_<pid>_<sequence>` schema, puts it first on `search_path`, and applies the full DB2 schema
there. The intended safety property is that unqualified eval tables shadow production tables while
`public` remains available for pgvector and pg_trgm types.

The production schema deliberately addresses objects as `public.<name>` at roughly 950 sites. On a
fresh database those references cannot see tables created in the private schema. The first observed
failures are:

~~~text
ERROR: relation "public.kb_admin_grant" does not exist
ERROR: relation "public.kb_vault_rewrap_operation" does not exist
~~~

Disabling function-body checks only postpones the first failure; guarded `DO` blocks still compile
against missing `public` relations. Removing the qualifiers is forbidden. At least one qualifies the
`kb_principal_is_admin()` lookup used by tenant-write RLS policy, preventing attacker-controlled
`search_path` resolution from substituting an admin table. A dev-only benchmark harness must not
weaken that production security boundary.

Applying the schema directly into `public` could make the old C path run, but would preserve a
second schema/bootstrap entry point and a string-only `AIMEE_DB2_EVAL_URL` safety contract. The Go
retirement path replaces that ambiguity with a typed lease and lets ordinary memory startup exercise
the same schema path as production in an isolated database.

## Current caller inventory

A source scan finds eleven direct `mem_eval_open_temp_db()` call sites:

- nine LoCoMo/LongMemEval run and diagnostic paths in
  `src/modules/benchmarks/agent_eval_benchmarks.c`;
- `mem_eval_load_corpus` in `agent_eval_memory_support.c`; and
- `test_fusion_surfaces_bridged_memory` in `src/tests/test_memory_retrieval_eval.c`.

The nine dataset paths are `mem_eval_run_locomo`, `mem_eval_run_locomo_session_support`,
`mem_eval_run_locomo_qa`, `mem_eval_report_locomo_qa_failures`, `mem_eval_report_locomo_misses`,
`mem_eval_run_longmemeval`, `mem_eval_run_longmemeval_qa`,
`mem_eval_report_longmemeval_qa_failures`, and `mem_eval_report_longmemeval_misses`.

The earlier rejection described the machinery as having one consumer. That count was incorrect and
must not be used as deletion evidence. Retirement is gated on a mechanical zero-caller check, not a
hand-maintained inventory.

## Ownership and safety invariants

1. `server-go/modules/benchmarks` owns benchmark-run identity, lease validation, bounded lifecycle,
   cancellation, and cleanup orchestration.
2. Memory owns schema application, storage, ingest, retrieval, and deletion semantics. Benchmarks
   consumes a versioned memory boundary; it does not import or duplicate DB2 internals.
3. A run starts only with a lease binding a unique run ID, database identity, creation time,
   expiration, and cleanup authority. A raw DSN is not a lease.
4. Provisioning fails closed for a production endpoint, an existing or non-empty database, a reused
   database identity, an expired lease, or an identity that cannot be verified independently of the
   caller-supplied DSN.
5. Cleanup drops only the database whose verified identity matches the active lease. Missing,
   malformed, expired, or mismatched cleanup authority leaks the disposable resource for operator
   recovery rather than guessing and risking another database.
6. Case runners receive a memory client, not provisioning credentials or a database handle. They
   cannot apply schema, alter routing, or reach production memory as a fallback.
7. Cancellation and failure stop the isolated service, attempt lease-scoped cleanup, and emit a
   terminal result that distinguishes provisioning, startup, execution, and cleanup failures.
8. The `public.` qualifiers, RLS policy, and production schema remain unchanged by this work.

## Plan

### 1. Define the Go disposable-run boundary

Add package-private lifecycle types to `server-go/modules/benchmarks`, including a `RunID`, an opaque
database identity, a bounded lease, and interfaces for provisioning an isolated memory endpoint and
revoking the leased resource. Construction validates every invariant before returning a case-facing
memory client.

The provisioner must attest that it created a new empty database on an explicitly configured
disposable service. It must compare the resolved target against the configured production target and
verify server/database identity after connecting; textual DSN inequality is insufficient. The
production DSN is never used as a default.

The lifecycle owns one state machine: provisioned, memory-ready, running, cleaning, and terminal.
Retries are idempotent by run ID, never silently adopt a pre-existing database, and never widen
cleanup authority. Credentials and DSNs are redacted from results and logs.

### 2. Exercise ordinary memory startup in isolation

Start the memory-serving process with the leased database as its only storage target. Its normal
startup path applies the production schema into that database's `public` schema. Readiness must prove
both the leased database identity and the memory endpoint's run binding before any corpus row is
ingested.

Benchmark setup, cases, and assertions use memory ingest/retrieval APIs. There is no benchmark-only
schema loader, `search_path` mutation, direct DB2 query, or fallback to a process-global connection.
This preserves storage ownership while testing the boundary a deployed consumer uses.

### 3. Migrate or remove every legacy caller

Move the dataset callers to the separate Go direct-track residual rather than translating their C
DB access. Port the fusion integration fixture to the disposable Go lifecycle so it still proves a
graph-only bridge through the real memory boundary. Replace or remove `mem_eval_load_corpus` once no
retained caller needs its process-global scratch state.

Add a mechanical checker that rejects new references to `mem_eval_open_temp_db`,
`mem_eval_close_temp_db`, `db2_eval_open_temp_store`, `db2_eval_close_temp_store`, or
`AIMEE_DB2_EVAL_URL` outside the explicitly staged migration allowlist. Shrink the allowlist with
each caller migration; completion requires it to be empty.

### 4. Delete the broken scratch-store implementation

After the zero-caller check passes, delete the declarations and implementations in
`agent_eval_internal.h`, `agent_eval_memory_support.c`, `modules/db2/c/eval_support.h`, and
`modules/db2/c/db2_init.c`.
Remove the private-schema name, `search_path` manipulation, eval connection takeover, eval-specific
schema application, and obsolete Makefile commentary.

Retain production schema qualifiers and all memory/RLS behavior unchanged. Any DB2 compatibility
code still required by production is outside this deletion.

## Failure and evidence behavior

Each run records bounded, non-secret evidence for run ID, verified disposable database identity,
lease timestamps, memory readiness, case counts, cancellation, and cleanup outcome. A cleanup
failure is terminal and visible even when benchmark scoring succeeded. Incomplete or unclean runs
cannot update a baseline or report a pass.

Tests use a fake provisioner for state-machine failures and an integration provisioner for real
database identity, schema startup, memory-boundary traffic, and cleanup. A sentinel production
database is observed before and after the integration suite to prove that neither data nor schema
changed.

## Acceptance

These are proposed-new TDD targets for the pending implementation. The checker and named tests are
delivery artifacts of that implementation; this documentation-only corrective PR does not claim
that they exist or pass yet:

~~~yaml acceptance
- {id: 1, tier: mechanical, check: "python3 scripts/check_eval_temp_store_retired.py"}
- {id: 2, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestDisposableRunLease"}
- {id: 3, tier: unit, check: "cd server-go && go test ./modules/benchmarks -run TestDisposableRunRefusesUnsafeTarget"}
- {id: 4, tier: integration, check: "cd server-go && go test ./modules/benchmarks -run TestDisposableRunUsesMemoryBoundary"}
- {id: 5, tier: integration, check: "cd server-go && go test ./modules/benchmarks -run TestDisposableRunCancellationCleansOnlyLeasedDatabase"}
~~~

Completion additionally requires:

- the caller/identifier checker has an empty migration allowlist;
- the eleven current C call sites are migrated or deleted;
- the shadow-schema/search-path implementation and `AIMEE_DB2_EVAL_URL` contract are gone;
- the real integration fixture proves normal memory startup and ingest/retrieval in the leased
  database, then proves lease-scoped cleanup;
- unsafe, reused, non-empty, ambiguous, expired, and mismatched targets fail closed; and
- a sentinel production database is byte- and schema-equivalent before and after all success,
  cancellation, timeout, and cleanup-failure cases.

## Non-goals

- Rewriting, de-qualifying, or making the production DB2 schema relocatable.
- Giving the benchmarks module schema, SQL, or memory-retrieval ownership.
- Restoring the removed standalone C eval executables.
- Implementing the dataset-specific Go runner or choosing its result schema; that belongs to the
  separate direct-track residual.
- Changing production RLS, memory ranking, embedding, routing, or baseline policy.
- Claiming that existing live `memory.benchmark` or smoke coverage replaces disposable-corpus
  integration evidence.
