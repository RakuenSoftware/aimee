# SQLite WORM Audit Worker

Both `aimee-server` and `aimee-kb` use
`src/modules/audit/audit_worm.c` as their SQLite WORM implementation. They use
separate files, keys, and processes: the server keeps its existing audit store,
while the KB worker defaults to `$AIMEE_HOME/audit/kb-worm-live.db`. Neither
store runs on, calls, or depends on the tap.

## Transaction contract

The producer transaction inserts its application mutation and one immutable
`kb_audit_outbox` row. PostgreSQL commits both or neither. `pg_notify` wakes the
worker after commit, while polling recovers notifications lost during downtime.

`aimee-kb-worm` claims committed, undelivered intents with `FOR UPDATE SKIP
LOCKED`. It appends each event to SQLite with the stable ID `kb:<outbox_id>`,
records the returned SQLite sequence in `kb_audit_delivery`, then commits the
PostgreSQL claim transaction.

SQLite and PostgreSQL cannot share one atomic transaction. The bridge therefore
uses idempotent retry: a crash after the SQLite commit but before PostgreSQL
acknowledgement leaves the intent pending; retry finds the byte-identical SQLite
event and returns its original sequence. Reusing an event ID with different
evidence fails closed. The delivery sequence is unique, so starting against an
empty or stale SQLite file after prior deliveries also fails closed instead of
silently forking the chain.

The PostgreSQL outbox and delivery ledger reject update, delete, and truncate.
SQLite rejects row updates and deletes, verifies every predecessor/hash link,
and adds keyed checkpoints. Filesystem immutable flags are best-effort; the
cryptographic chain and independently operated tap evidence are the portable
controls.

Both processes call the same startup-admission function before accepting work.
It opens and verifies the complete existing SQLite chain and every checkpoint
MAC, checkpoints an intact non-empty head, and requires the result to be GREEN.
`aimee-server` fails before creating its module bus or API sockets when this
admission fails; `aimee-kb-worm` fails before claiming PostgreSQL outbox work.

## Privilege boundary

Provision `schema_roles.sql`, `schema.sql`, and `schema_grants.sql` in that
order. They create the `aimee_kb_worm_worker` login and reduce it to:

- `USAGE` on the isolated `aimee_kb_worm_api` schema;
- `EXECUTE` on `aimee_kb_worm_api.claim(integer)` and
  `aimee_kb_worm_api.ack(bigint,bigint)`.

It has no `USAGE` on `public` and no table, sequence, application-function, or
status-function privileges. Conversely, `aimee_kb_runtime` may execute
`kb_audit_worm_submit(...)` but cannot manipulate either ledger or invoke the
worker API. PostgreSQL no longer creates or owns a KB WORM chain table.

Give the worker a credential distinct from the application runtime `AIMEE_STORE_URL`:

```sh
AIMEE_WORM_POSTGRES_URL='postgresql://aimee_kb_worm_worker:...@db/aimee' \
  AIMEE_HOME=/var/lib/aimee-worm \
  AIMEE_WORM_PATH=/var/lib/aimee-worm/audit/kb-worm-live.db \
  aimee-kb-worm
```

The worker has no runtime-DSN fallback. It exits unless the authenticated role
is exactly `aimee_kb_worm_worker`, has no membership edges or administrative
attributes, cannot resolve `public`, and can resolve only the isolated worker
API. It also holds a PostgreSQL session advisory lock, preventing two workers
from splitting one logical chain across different SQLite files.

Run exactly one worker as a separate OS service or container, with its SQLite
directory on persistent storage and mode `0700`. A thread inside `aimee-kb` is
not a security boundary because it shares address space, credentials, control
flow, and crash fate with the code being audited. The supplied systemd unit and
`deploy/compose/worm-worker.yaml` allocate dedicated persistent worker state.

## Upgrade from the retired PostgreSQL chain

Schema upgrades stop creating or writing `kb_audit_event`, but deliberately do
not drop an existing table: silently deleting prior evidence would violate the
purpose of WORM. Preserve/export and verify the legacy chain, start the SQLite
worker, confirm the outbox reaches zero and the SQLite chain verifies, then
retire the inert PostgreSQL table under an explicit evidence-retention policy.
Do not treat the outbox or delivery ledger as the evidence chain.

## Operations

Monitor `kb_audit_worm_pending()` from the runtime health path. Alert on both
pending count and oldest age. The worker uses transactional `LISTEN/NOTIFY` for
normal wakeups and a one-second poll for recovery.

```sh
# Drain all currently available work, then exit.
aimee-kb-worm --once --batch=1000

# Normal service tuning.
aimee-kb-worm --batch=128 --poll-ms=1000
```

The real-PostgreSQL recovery and privilege proof is
`scripts/run-worm-worker-pg-test.sh`. The static regression gate is
`make -C src worm-worker-boundary-check`.

## Trace a memory mutation to delivery

New shared-memory mutation intents include `changeset_id` and `evidence_event_id` in
content-free outbox metadata. The event ID identifies the individual mutation when a
changeset contains several rows. Older intents have only the changeset ID; do not infer
an exact event match for those rows.

An operator with read access can inspect a known changeset using `psql -v changeset_id=...`:

```sql
WITH intents AS (
  SELECT o.*, CASE WHEN detail IS JSON OBJECT THEN detail::jsonb END AS metadata
  FROM kb_audit_outbox o WHERE action LIKE 'memory.%'
)
SELECT i.metadata->>'changeset_id' AS changeset_id,
       i.metadata->>'evidence_event_id' AS evidence_event_id,
       i.outbox_id, 'kb:' || i.outbox_id AS worm_event_id,
       d.audit_seq, d.sealed_at,
       CASE WHEN d.outbox_id IS NULL THEN 'pending' ELSE 'delivered' END AS delivery,
       e.event_id IS NOT NULL AS detailed_evidence_retained
FROM intents i
LEFT JOIN kb_audit_delivery d USING (outbox_id)
LEFT JOIN memory_evidence_events e
  ON e.event_id=i.metadata->>'evidence_event_id'
 AND e.changeset_id=i.metadata->>'changeset_id'
WHERE i.metadata->>'changeset_id'=:'changeset_id'
ORDER BY i.outbox_id;
```

Run this in a separate transaction after the writer commits. A visible intent proves
committed intent, and a delivery receipt records the worker's acknowledgment. Verify the
SQLite chain and the matching `kb:<outbox_id>` event before claiming chain verification.
An empty result does not prove that no mutation happened: check placement, changeset,
retention, and legacy metadata first. This query describes KB delivery; Server has its own
private audit store.

Worker downtime leaves committed intents pending. An outbox insertion failure aborts
the originating mutation. Erasing detailed evidence does not erase immutable identifiers
or delivery receipts, but those receipts cannot reconstruct erased memory content.
