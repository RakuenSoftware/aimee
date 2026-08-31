# Memory Changeset WORM Seal: Validation Report

Every path that closes a fact-graph changeset now writes an immutable audit
intent inside the mutation transaction. A separately credentialed
`aimee-kb-worm` process turns committed intents into the hash chain. Five SQL
close paths previously closed a changeset without producing any audit record.

## What was already true

Before this follow-up, the C mutation API sealed its own closes synchronously.
`fm_commit_finish()` called `db2_kb_audit_append_in_txn()` on every close, and
the revert and ingest-rollback paths routed through it. That call was
unconditional: it did not consult `config.audit_worm_enabled`.

The gap was not the C API. It was the SQL side.

## The defect

Five functions in `schema.sql` open a changeset, apply it, and close it with no
audit append:

| function | close | reached by |
|---|---|---|
| `evidence_object_mutation` | trigger-owned changeset | every direct write to `memories`, `docs`, `document_versions` |
| `knowledge_changeset_revert` | its own reverting changeset | changeset revert |
| `document_lifecycle_apply` | its own changeset | document lifecycle transitions |
| `operator_review_decide` | its own changeset | an operator verdict on a review item |
| `ontology_package_migrate` | its own changeset | ontology package migration |

All five run as the runtime role, and `kb_audit_worm_append` is deliberately not
granted to runtime, so a direct call was not open to them. The omission was
therefore structural rather than accidental: there was no seam they could use.

No unit test could observe it. The sqlite shim (`schema_sqlite.sql`) mirrors
`fact_graph_commits`, `fact_graph_changes`, `kb_audit_outbox` and the semantic
mutation guards, but carries none of these five functions and no evidence
trigger. The unsealed close does not exist under the shim, so there is nothing
there to fail.

## The change

`kb_fact_commit_worm_seal(commit_id, subject)` is a `SECURITY DEFINER` function
that reads the actor, authority, operation and status from the changeset row and
submits one immutable outbox intent. Every audited field comes off the row
rather than from the caller, which makes the changeset seal truthful even when
runtime invokes it.

The follow-up removes chain construction from every PostgreSQL producer:

- `kb_audit_worm_submit` inserts into append-only `kb_audit_outbox` and emits a
  transactional notification.
- `db2_kb_audit_append_in_txn` submits to that function in production; the C
  process no longer reads the chain head, takes its lock, computes its hash, or
  inserts an immutable `kb_audit_outbox` intent.
- `aimee-kb-worm` is a separate libpq-only process requiring
  `AIMEE_WORM_DB2_URL`; it refuses to reuse the ordinary runtime credential.
- `aimee_kb_worm_worker` has no access to the application `public` schema and
  no table or sequence privileges. Its only capability is the bounded
  isolated `aimee_kb_worm_api.claim(limit)` / `ack(outbox_id,seq)` definers.
- Drain, chain append, witness append, and immutable delivery acknowledgement
  commit together. A crash leaves either all of them or none of them.

The worker preserves the established canonical row bytes — same action and
same `commit_id=<id>` detail — so the shared SQLite worker continues to
verify the resulting trail.

## Verified

- `make changeset-worm-seal-check`: 6 unit tests pass; the check resolves 5
  close sites in `schema.sql`, all sealed.
- The gate reports the real defect. Run against the pre-change schema at
  `origin/testing`, it resolves the same 5 sites and reports **5 of 5 unsealed**,
  naming each function and line. Against the post-change schema, 0 of 5.
- The gate cannot pass vacuously. It exits 2 when it resolves zero close sites,
  and a unit test asserts that. A second test deletes the real seal call and
  asserts the check reports it.
- The gate does not over-demand: a close that targets another changeset
  (`knowledge_changeset_revert`'s update of the changeset being reverted) is not
  required to seal on its own, since that transition is audited as the subject of
  the reverting changeset's seal. A unit test pins that.
- `schema-sync-check`, `db2-contract-check`, `db2-declaration-ledger-check`,
  `db2-activation-check`, `line-check` pass.
- `make worm-worker-boundary-check`: 7 unit tests plus a structural gate pin
  the runtime revoke, enqueue-only C production branch, narrow fact seal,
  separate credential requirement, isolated API schema, and one-object worker
  link surface.

## PostgreSQL gate

Executed on 2026-08-25 against `pvetest` (`192.168.1.252`), PostgreSQL 17.11,
with pgvector 0.8.0 and pg_trgm 1.6 available. The connection was
`postgresql:///postgres` under the `postgres` OS account, using peer
authentication. The harness created isolated databases for the live and
fault-injection arms and removed both through its exit trap.

The live arm inserts a memory through `evidence_object_mutation`, drains its
committed intent, asserts that every closed `memory.%` changeset carries a
matching WORM row, checks that the row records `worm-seal-tester/user` from the
changeset, and re-verifies the hash chain. The negative control applies the
same schema with the five seal calls stripped and requires the matching count
to fall to zero.

Command:

```sh
su postgres -c "bash /opt/worm-seal-test/scripts/run-fact-mutation-pg-test.sh postgresql:///postgres"
```

Result (exit 0):

```text
check_changeset_worm_seal: 5 close sites, all sealed
check_changeset_worm_seal self-test: PASSED
fact mutation PostgreSQL gate: PASSED
  memory changeset seal: 1 of 1 closed changesets carry a WORM row
  negative control (seal calls stripped): 0 of 1
```

The `0 of 1` negative control demonstrates that the live assertion matches the
new seal wiring rather than an unrelated audit row.

## Worker isolation and recovery gate

`scripts/run-worm-worker-pg-test.sh` was also executed on the same PostgreSQL
17.11 host. It applies the hardened roles and grants, submits as
`aimee_kb_runtime`, and runs the binary as `aimee_kb_worm_worker`.

The gate proves runtime has no INSERT on the chain, outbox, or delivery ledger
and cannot execute the internal appender or drainer. It also proves that the
worker cannot resolve `public` at all, closing PostgreSQL's default-EXECUTE
function leak, and can resolve only its one-function API schema. It then injects
a failure after chain insertion but before delivery acknowledgement. The
complete drain transaction rolls back, the two intents remain pending, restart
seals them exactly once, and a second restart is a no-op.

```text
WORM worker PostgreSQL gate: PASSED
  producer: submit only; chain/outbox/delivery writes denied
  worker: bounded drain only; crash rollback and retry verified
  chain: 3 rows + 3 witnesses, idempotent restart, 0 broken links
```

The request path now pays only for the durable intent insert. Chain hashing,
witnessing, and their advisory locks are owned by the worker. The operational
capacity measures are pending-intent count/age and worker throughput, not
foreground mutation latency.
