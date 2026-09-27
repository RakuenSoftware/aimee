# Conditional shared lifecycle versions — 2026-09-23

Continues PR #2990 from `0040cfcbe196ddd15681d220f2d98afbdd29681f`.

## Behavior

Shared `reject` and `restore` accept an optional `expected_version`. The shared Go
owner locks the RLS-visible canonical row and compares owner, exact record ID and
revision before calling the existing rejection/restoration implementation. The
lock remains in the request transaction through tombstone, revision and audit
writes. Hidden/missing IDs remain `not_found`; stale or wrong-owner observations
return `conflict` with `expected_version_conflict`. Restoration still requires an
authenticated caller and the existing tombstone ownership policy.

The C server and KB console forward opaque preconditions and unsupported retry
keys. Malformed/unsupported console conditions return 400; conflicts return 409.
The external-host ownership ledger was reviewed and refreshed for the server
adapter: it only copies opaque JSON fields; all validation and locking remain
in Go. No C memory policy or event-bus implementation was added. Unversioned success
paths remain unchanged. At this checkpoint lifecycle idempotency keys were explicitly refused.
The subsequent [retry slice](memory-lifecycle-retries-2026-09-23.md) adds them.
This does not certify MR-02, all lifecycle transitions, restore-resistant owner
identity, durable retries or final provider-release correctness.

## Validation

Using Go 1.26.7 and a disposable PostgreSQL 18.6 instance with pgvector, separate
from the production databases:

- `go test ./modules/memory/... -count=1`: passed, including the real PostgreSQL
  fixtures and restricted-runtime-role replay against the shipping shared schema.
- `go test -race ./modules/memory -run 'Test(LifecycleVersion|ExpectedVersionConcurrent|MutationPublicContextBoundary)' -count=1`: passed.
- The lifecycle replay covers malformed conditions, explicit unsupported retry
  refusal, stale rejection after a scope-tag revision, wrong owner, hidden reject
  and restore targets, stale restoration, successful transitions, and stale replay
  after restoration.
- A real two-connection test observes the waiting backend in `pg_blocking_pids`:
  rejection waits behind a competing correction and then refuses its old version.
- `unit-test-server-memory-get`: passed, preserving exact decimal-string versions
  and explicit null retry options through the native restoration adapter.
- `unit-test-kb-http-routes`, with the actual Go fixture selected by
  `AIMEE_TEST_RUNTIME_FIXTURE`: passed, including console precondition forwarding
  and 400/409 refusal status handling.

`AIMEE_DB2_REPLAY_URL` selected the disposable shipping-schema database;
`AIMEE_MEMORY_EVAL_URL` selected a separate empty database for temporary-table
fixtures. No test DSN or credential is checked in. The KB HTTP executable uses the
existing native test fixture; these results do not claim a new deployed PR image.

## Concurrent 0.4.5 upgrade validation

CT100 on `192.168.1.253` was upgraded in place from 0.4.4 to the published 0.4.5
application, PostgreSQL and embedder images. Existing Compose topology and volumes
were retained. Rollback material is the stopped-stack snapshot
`pre-aimee-045-20260923` plus `/opt/aimee/backups/pre-045` (configuration and a
PostgreSQL custom-format dump). The application image is
`sha256:36f866e8ce4ce45d28b1d68b5ab5295c5e07bff81a9cc9d83d04df31e10187ba`.

During this implementation, all three production services remained healthy on
0.4.5. Repeated thin-client checks reported authorized HTTPS/mTLS and server
`state: ok`, with capture healthy. A private-memory smoke record was stored,
read back, deleted, and subsequently returned `not_found`. The earlier controlled
restart retained the database, dashboard login and client identity; readiness
reported database, retrieval and modules healthy.

The optional KB was already disabled and remains disabled. Consequently the
shared-memory changes above were tested in isolation, not deployed to CT100.
The existing audit amber state for newly uncheckpointed records is not presented
as a green audit certification. These checks validate the observed upgrade and
client operations, not the full release topology/performance acceptance matrix.
