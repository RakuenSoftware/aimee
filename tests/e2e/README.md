# Postgres end-to-end suite

`make unit-tests` runs every C test against the in-memory sqlite shim
(`db2_test_shim_open`). Production is Postgres via libpq, and sqlite accepts SQL
that Postgres rejects, so a green unit run is not evidence that this
subsystem's SQL executes at all.

That gap was not hypothetical. `db2_entity_edge_two_hop_neighbors` built an
unparenthesised per-branch `LIMIT` inside a `UNION`, which Postgres rejects as a
syntax error. The function had no production caller and its tests ran on the
shim, so it had never executed against the real database for as long as it had
existed. Two further defects (weight normalisation rewriting typed-fact
confirmation counts, and a co-occurrence upsert bumping a fact's weight) only
appear in a real maintenance cycle, where the lifecycle jobs, the orphan prune
and normalisation all touch the same rows in one pass.

This suite exists to close that gap for the typed-fact knowledge layer.

## Running it

Use a throwaway host or container. The suite writes and deletes rows, and runs
a real maintenance cycle, so do not point it at anything you care about.

```
# 1. inside the throwaway box, as root
tests/e2e/provision-pg-env.sh

# 2. build
cd src && make -j$(nproc) server all

# 3. let aimee-kb apply the schema itself (see the warning below)
./aimee-kb --http-port=8911

# 4. run
tests/e2e/typed-facts-pg-e2e.sh
```

Every assertion prints PASS or FAIL and the script exits non-zero if any
failed.

## Two things that will waste your time otherwise

**Do not hand-apply `schema.sql` with `psql -f`.** It is a template. The service
substitutes `__EMBED_DIM__` from the configured embedder width at init and
writes bookkeeping rows psql never will. A raw apply leaves
`kb_meta.schema_embedding_dim` holding the literal string `__EMBED_DIM__`, so
`aimee-kb` refuses to start, and leaves `rel_types` unseeded, so every
typed-fact operation fails in a way that reads exactly like a product bug.

**`aimee-kb` exits immediately without `--http-port=N`.** HTTP is its only
transport. When it is not running, every kb-backed route answers
`"the knowledge service refused"`, which is indistinguishable from a real
defect. Section 0 of the suite therefore asserts liveness and exercises a
known-good read and a known-good op before any other assertion runs. A red
result below section 0 means nothing until section 0 is green.

## What it covers

| Section | Behaviour |
| --- | --- |
| 0 | Harness liveness, and a control on each of the two services |
| A | Typed facts bridge the recall walk; superseded and tombstoned facts do not; co-occurrence still bridges |
| B | Listing surfaces still return the co-occurrence population only |
| C | The two section 5 lifecycle jobs fire in a real maintenance cycle |
| D | Weight stays a confirmation count: not normalised, not bumped by co-occurrence |
| E | Retract and entity merge/unmerge, including authority, immutable and hard-delete behaviour |
| F | PII-tier relations still participate in the walk, per the section 7 decision |
| G | The lifecycle still runs with `typed_facts_enabled: false` |
