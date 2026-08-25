# WORM Audit Worker

WORM chain construction is an independent security compartment. Application
processes submit immutable audit intents; they do not hold chain-writer
authority or the worker credential.

## Transaction contract

The producer's transaction inserts its application mutation and one
`kb_audit_outbox` row. PostgreSQL commits both or neither. `pg_notify` wakes the
worker only after commit, while polling recovers notifications lost during
downtime.

`aimee-kb-worm` claims committed, undelivered intents with `FOR UPDATE SKIP
LOCKED`. One drain transaction appends the hash-chain row, appends its vault
witness, and inserts `kb_audit_delivery`. A crash before commit rolls back all
three. Restart sees the still-pending intent and retries it; a delivered intent
cannot be appended twice.

The outbox, delivery ledger, and chain all reject update, delete, and truncate.
Only schema-owner intervention that deliberately disables those triggers can
rewrite history.

## Privilege boundary

Provision `schema_roles.sql`, `schema.sql`, and `schema_grants.sql` in that
order. They create the `aimee_kb_worm_worker` login role and reduce it to:

- `USAGE` on the isolated `aimee_kb_worm_api` schema;
- `EXECUTE` on `aimee_kb_worm_api.drain(integer)`.

It has no `USAGE` on the application `public` schema and receives no table,
sequence, application-function, status-function, or internal-appender
privileges. This matters because PostgreSQL grants new functions to `PUBLIC` by
default: removing schema usage prevents a later application function from
silently becoming worker-reachable. Conversely, `aimee_kb_runtime` can execute
`kb_audit_worm_submit(...)` but cannot insert the chain, manipulate either
ledger, or invoke the drainer.

Give the worker a credential distinct from `AIMEE_DB2_URL`. Start it with:

```sh
AIMEE_WORM_DB2_URL='postgresql://aimee_kb_worm_worker:...@db/aimee' \
  aimee-kb-worm
```

The worker intentionally has no fallback to the runtime DSN and exits unless
the authenticated PostgreSQL role is exactly `aimee_kb_worm_worker`, has no
membership edges or administrative attributes, cannot resolve `public`, and
can resolve the isolated worker API.

Run it as a separate OS service or container. A thread inside `aimee-kb` is not
a security boundary: it would share address space, process credentials, control
flow, and crash fate with the operations being audited. The container image
ships both executables so a hardened deployment can run the same image with an
overridden entrypoint of `/usr/local/bin/aimee-kb-worm` and only the worker DSN.
`deploy/compose/worm-worker.yaml` provides that hardened overlay for an
externally reachable DB2. The self-contained KB image's embedded PostgreSQL is
a development/single-owner tier, not a process-isolated hardened deployment.

## Operations

Monitor `kb_audit_worm_pending()` from the runtime health path. Alert on both
pending count and oldest age; a small count with an old intent still means the
audit observer is stalled. The worker uses transactional `LISTEN/NOTIFY` for
normal wakeups and a one-second poll for recovery.

Useful controls:

```sh
# Drain all currently available work, then exit.
aimee-kb-worm --once --batch=1000

# Normal service tuning.
aimee-kb-worm --batch=128 --poll-ms=1000
```

The real-PostgreSQL recovery and privilege proof is
`scripts/run-worm-worker-pg-test.sh`. The static regression gate is
`make -C src worm-worker-boundary-check`.
