# Memory Changeset WORM Seal: Validation Report

Every path that closes a fact-graph changeset now appends a hash-chained audit
row for it, inside the same transaction as the mutation. Five SQL close paths
previously closed a changeset leaving nothing in `kb_audit_event`.

## What was already true

The C mutation API has always sealed its own closes. `fm_commit_finish()`
(`src/modules/db2/c/fact_mutation.c:445-465`) calls
`db2_kb_audit_append_in_txn()` on every close, and the revert and
ingest-rollback paths route through it (`fact_mutation.c:1791`, `2004`). That
call is unconditional: it does not consult `config.audit_worm_enabled`, which is
default-off and gates only the artifacts and guardrails seams. A memory
mutation through the C API was therefore already tamper-evident, chained, and
verifiable by `db2_kb_audit_verify_chain()`.

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
`fact_graph_commits`, `fact_graph_changes`, `kb_audit_event` and the semantic
mutation guards, but carries none of these five functions and no evidence
trigger. The unsealed close does not exist under the shim, so there is nothing
there to fail.

## The change

`kb_fact_commit_worm_seal(commit_id, subject)` is a `SECURITY DEFINER` function
that reads the actor, authority, operation and status from the changeset row and
appends one WORM record for it. Every audited field comes off the row rather
than from the caller, which is what makes it safe to grant to runtime: runtime
can cause a truthful seal for a changeset that exists, and cannot forge a row
that says anything else. `kb_audit_worm_append` stays ungranted to runtime.

The row is shaped identically to the one `fm_commit_finish()` writes — same
action, same `commit_id=<id>` detail — so one verifier reads memory's audit
trail whichever path produced the mutation.

## Verified

- `make changeset-worm-seal-check` — 6 unit tests pass; the check resolves 5
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

## PostgreSQL gate

Executed on 2026-08-25 against `pvetest` (`192.168.1.252`), PostgreSQL 17.11,
with pgvector 0.8.0 and pg_trgm 1.6 available. The connection was
`postgresql:///postgres` under the `postgres` OS account, using peer
authentication. The harness created isolated databases for the live and
fault-injection arms and removed both through its exit trap.

The live arm inserts a memory through `evidence_object_mutation`, asserts that
every closed `memory.%` changeset carries a matching WORM row, checks that the
row records `worm-seal-tester/user` from the changeset, and re-verifies the hash
chain. The negative control applies the same schema with the five seal calls
stripped and requires the matching count to fall to zero.

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

## Cost

`kb_audit_worm_append` takes the audit chain's advisory transaction lock and
also appends a witness row, so a changeset close now serializes against every
other audit appender. The close is one row per changeset, not per changed fact,
so this is one serialized append per mutation batch. A bulk import that writes
each row in its own changeset pays it per row; staging a changeset around the
batch collapses it to one. Worth measuring on an ingest run before enabling
hardened-tier deployments at volume.
