# Postgres end-to-end suite

Three suites live here, each with its own section below:
`typed-facts-pg-e2e.sh`, `module-liveness-pg-e2e.sh`, and
`learning-loops-pg-e2e.sh`. All three need a throwaway box with real Postgres;
none of them run under `make unit-tests`, which is the point.

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

# Module liveness and provider registration

`module-liveness-pg-e2e.sh` answers a different question: does a deployed daemon
come up with its modules attached and its code paths live? It also points the
KB capture layer at a real read-only filesystem while leaving PostgreSQL
available, proving that health rejects a completeness claim and the WORM ledger
retains the gap.

The gap it was written for. Signal capture is served by `aimee-kb` -- that is
where the learning tables live -- and the router it calls needs a signal
classifier to decide which sinks a signal reaches. Only `aimee-server` ever
registered one. In the KB the pointer was null, every signal was refused with a
single WARN, and the route answered `200` carrying an error document while
writing nothing. Signal ingest through the KB had never worked.

No unit test could have caught it: every test registers its own provider, so
none can observe that production does not. `make lint` now gates the shape
(`scripts/check_provider_registration.py`), but a gate reads source. It cannot
tell you a deployed daemon actually attached its modules.

The harness that originally found the bug started **two** modules, and that is
part of why it hid: a module which is granted but never attached fails exactly
like a module that was never placed. This suite attaches every module each
daemon is granted and has a binary for -- 7 on the KB, 19 on the server -- and
reports any it could not start rather than skipping it quietly.

## Running it

```
# in a throwaway box, with Postgres up and an empty aimee_shared database
cd src && make -j$(nproc) all
make build/obj/aimee-module build/obj/aimee-module-config build/obj/aimee-module-db1

AIMEE_ROOT=/path/to/aimee AIMEE_SRC=/path/to/aimee/src \
  tests/e2e/module-liveness-pg-e2e.sh
```

`AIMEE_DB2_URL` must reach the same database. The **postgres module** connects
by that URL rather than by the libpq defaults `aimee-kb` itself uses, and with
it unset the KB publishes the blocker *"store unavailable: the KB database
schema is not ready"* while it is visibly storing and retrieving. That is an
under-configured environment, not a defect, and section 5 asserts the service
does not contradict itself this way.

## What it covers

| Section | Behaviour |
| --- | --- |
| 0 | Every module the KB is granted deploys, attaches, and is still alive |
| 0a | Disabled capture is false in health and leaves a PostgreSQL `bus.capture.gap` row |
| 1 | The same for the server, with the KB reachable |
| 2 | The surfaces answer; a probe naming a command that does not exist fails the run |
| 3 | Signal capture is recorded, and a later commit supersedes the earlier one |
| 4 | Neither log reports a missing provider, unserved stage, or rejected grant |
| 5 | The service does not contradict itself about its own store; nothing crashed |

## It is tested against the bug

Deleting the KB's `learning_router_register_signal_classifier` line reproduces
the original defect, and four independent assertions catch it -- both captures
refused, no supersession recorded, and the log sweep quoting the original WARN:

```
  FAIL  signal 1 was refused -- the classifier is null again
  FAIL  no supersession was recorded by this run (was 1, now 1)
  FAIL  kb.log: 2 line(s) report a missing provider or stage
          WARN  learning: signal classification unavailable; refusing signal type=mark_rule
```

Section 3 judges the **delta** a run produces, not the state it inherits. The
first version asked "is there a superseded row?", which passed on a row left by
an earlier run -- and did exactly that in a run where every capture was refused
and nothing was written.

# The recursive self-improvement loops

`learning-loops-pg-e2e.sh` covers the learning loops themselves, and exists for
two failures that unit tests could not have caught.

**The producing halves shipped absent.** Nothing wrote a fate, so regret was
permanently zero and the detector bar never moved. No evidence probe was
installed, so the backlog drain refused to run. No sampler was registered, so
arm selection always fell back to its default. Every slice passed its unit tests
throughout, because a unit test can prove a consumer reads a row correctly
without ever asking whether anything writes one.

**The endogeneity gate could not see its own evidence.** It is a DB2 reader, and
DB2 lives in the KB; an earlier version ran in `aimee-server`, which builds with
`-DAIMEE_DB2_DISABLED`, so it reported "open" by never having consulted a ledger
at all.

## Running it

```
# throwaway box, Postgres up, an empty aimee_shared database
cd src && make -j$(nproc) all
make build/obj/aimee-module build/obj/aimee-module-config build/obj/aimee-module-db1

AIMEE_ROOT=/path/to/aimee AIMEE_SRC=/path/to/aimee/src \
  tests/e2e/learning-loops-pg-e2e.sh
```

It **deletes** the learning and curiosity tables it seeds.

## What it covers

| Section | Behaviour |
| --- | --- |
| 0 | Both services up, with the modules these loops need |
| 1 | An empty ledger leaves the gate open; a wholly self-referential one closes it; the daemon reports what the KB enforces |
| 2 | A closed gate admits nothing *and writes no task file*; real outside evidence reopens it |
| 3 | A later commit supersedes the earlier one unasked; an operator verdict reaches the ledger and counts as regret |
| 4 | The drain runs a real probe, and leaves an uncovered gap **open** rather than closing it by assertion |
| 5 | The policy layer answers with an arm this build declares |
| 6 | The fate ledger's SQL on real Postgres: one row per proposal, latest verdict wins, and the delimited `LIKE` refusing to count `reverted_by_operator` as `reverted` |
| 7 | Malformed and hostile input is refused rather than answered as though it parsed |
| 8 | Neither log reports a missing provider; neither service crashed |

Section 6 matters for the same reason the typed-fact suite does: those
statements run against the sqlite shim under `make unit-tests`, and sqlite
accepts SQL that Postgres rejects.

Section 7 pins one contract that otherwise reads as a bug: `--budget 0` is not
"do nothing". `curiosity_resolve_pass` treats any budget `<= 0` as unset and
substitutes its default, so an operator asking for none still gets a full pass.
