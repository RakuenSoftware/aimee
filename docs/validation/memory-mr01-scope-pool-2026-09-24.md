# MR-01/A4: live pooled request-scope isolation

The actual KB process passes 288 mixed-project HTTP requests with 12 concurrent
callers. Six interleaved cases cover each project's authorized record, each
cross-project refusal, a missing record, and a malformed governed timestamp
that forces a query error and transaction rollback. Sequential baseline and
post-workload checks also pass. The runner finished with exit zero and removed
its disposable stack.

[HTTP receipts](memory-mr01-scope-pool-2026-09-24/http/checks.json),
[live role evidence](memory-mr01-scope-pool-2026-09-24/http/runtime-role.json) and
[container image identities](memory-mr01-scope-pool-2026-09-24/http/image-identities.json)
are retained. The application was `7eb4cf3b1`, the same candidate that passed the
1714-check deployment matrix. PostgreSQL confirms live `aimee_store_runtime`
connections: this role neither owns the memories table nor has superuser or
RLS-bypass privileges. Fixture setup alone uses the owner connection.

This complements the [two-connection Go replay](memory-mr01-scope-pool-2026-09-24/race.txt),
which checks cleared transaction-local settings on both pooled connections.
The HTTP test checks actual cross-request behavior through the native host,
shared Go owner and PostgreSQL runtime connections. It uses an authorized
service credential with separately specified project audiences; the separate
[credential-scope regression](memory-mr01-serving-scope-2026-09-24.md) checks
project/workspace credential restrictions.

The first live attempt was interrupted by a local temporary-file quota that
closed its output stream after baseline and role checks. Removing redundant
task-generated Git transport bundles resolved that environment failure. The
complete rerun above supplies the passing receipt. No application or schema
changes were needed for this test. Other MR-01 closeout gates remain open.
