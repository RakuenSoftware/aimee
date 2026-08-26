# A schema change over a database that already exists

Record of validating the DB3 removal's teardown against real PostgreSQL, and of
the two defects that found. Reproduce with
`scripts/validation/db2-schema/upgrade-over-installed-schema.sh`.

## Why a separate run exists at all

Every other suite builds its database from scratch. That is right for a
template and useless for a teardown: a `DROP` that is wrong, or that silently
matches nothing, is indistinguishable from one that worked when there was
nothing there to drop. The sqlite shim does not execute this DDL as PostgreSQL
would, so the local suite cannot see it either.

The removal commit asserted that an installed deployment upgrades cleanly. It
did not, in two independent ways, and both were invisible until a real database
carrying the old schema was asked to take the new one.

## What was used

- **Host**: Proxmox at 192.168.1.252, CT 9107, fresh Debian 13.
- **PostgreSQL**: 17.11, pgvector 0.8.0, pgvectorscale 0.9.0.
- **Trees**: the removal commit and its parent, both deployed from tarballs.
- **Databases**: created `UTF8` with `TEMPLATE template0`.

## The defects

### 1. The upgrade aborted, so no deployment would have started

The trigger sweep matched:

```sql
AND (t.tgname LIKE 'db3\_capture\_%' ESCAPE '\\' OR ...)
```

PostgreSQL refuses a two-character escape string -- *"escape string must be
empty or one character"* -- and that error aborts the **entire schema apply**,
not just the sweep. Every installed deployment would have failed to start on
upgrade: strictly worse than the triggers being removed.

Now `LIKE 'db3%'`, which needs no escape at all. `_` remains a single-character
wildcard, which only widens the match inside a prefix nothing else uses.

### 2. Three functions survived, and the upgrade reported success

The teardown named an argument list per function. Three of seven were wrong:
`db3_admit_provider`'s last parameter is `boolean` rather than `bigint`, and
both `db3_enqueue_vector` forms begin with different types than were written.

The trap is that **`DROP FUNCTION IF EXISTS` with a wrong signature succeeds**.
It reports that nothing needed dropping. So the upgrade looked clean and left
`db3_admit_provider`, `db3_enqueue_vector` and `db3_enqueue_vector_to` behind.

Now dropped by name over `pg_proc` whatever the signature, which also covers a
deployment that upgraded through a version carrying different overloads.

## Result after the fixes

| step | result |
| --- | --- |
| old schema applied | 5 tables, 7 functions, 19 triggers |
| vector write on the old schema | lands |
| current schema applied over it | **applies** |
| removed objects | 0 tables, 0 functions, 0 triggers |
| vector write after the upgrade | lands |
| row written before the upgrade | survives |
| fresh install (no prior objects) | 0 objects; the DROPs are no-ops |
| index built with pgvectorscale present | 11 DiskANN, 0 HNSW |

## What the harness needs from the tree

`db2-test-template` drops and recreates by default, which is correct for a
template and defeats this test. It honours `AIMEE_TEMPLATE_KEEP` to apply in
place; that flag exists for this run and has no other caller.

## Not covered

The harness asserts on an object-name prefix (`PREFIX`, default `db3`). It
checks that named objects disappear and that the database still writes; it does
not diff the whole schema, so a change that removes something it was not told
about still passes. A future teardown should set `PREFIX` to its own and, if it
renames rather than removes, assert the new name is present as well.
