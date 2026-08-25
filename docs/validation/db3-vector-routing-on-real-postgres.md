# Validating DB3 vector routing against real PostgreSQL

The routed vector searches rest on a claim about SQL: that filtering on a
resolved generation selects the same rows as joining `projects` and comparing to
`current_generation`. A provider cannot join, so that reduction is the whole
reason a search can leave pgvector at all.

The sqlite shim the unit suite normally links cannot settle it. It translates
DB2's SQL rather than executing it as PostgreSQL would, and it understands
neither the `vector` type nor the `<=>` operator, so every pgvector statement
returns an error value and the tests only assert that nothing crashes. A
reduction that is wrong looks exactly as green under the shim as one that is
right.

This is the record of running the work on containers with real PostgreSQL 17
and pgvector, and of what that found: three defects that every existing check
called green, one whole half of the routed path with no coverage at all, and a
build edge that fourteen fixtures were missing.

## What was used

- **Host**: Proxmox `pvetest` at 192.168.1.252, `pve-manager/9.2.6`.
- **Containers**: CT 9100 `aimee-db3-verify` for the suites, and CT 9101
  `aimee-gen-verify` for the write-path fixture. Both created for this and
  destroyed afterwards. Debian 13 standard, unprivileged, DHCP, 6 cores / 12GB /
  32GB.
- **Database**: PostgreSQL 17.11 with `vector` 0.8.0 and `pg_trgm` 1.6, local to
  the container. Nothing pointed at a shared or production instance.
- **Toolchain**: gcc 14.2, Go 1.24.4, clang-format 19, all from Debian.
- **Tree**: this branch, built inside the container from a source tarball. No
  binaries were carried in.

The harness is `scripts/validation/db3/`. `provision-container.sh` builds the
container, `pg-setup.sh` creates the template database, `run-pg-tests.sh` runs
the C suite against libpq, `run-go-e2e.sh` runs the Go suite and the bus proof,
`run-generation-test.sh` runs the write-path fixture on a database of its own,
and `code_verify.sql` is the equivalence check described below.

## What it found

### The C tree did not build

`pgvec_db3.h` includes `<aimee/db2/db3_route.h>`, and the include path that
makes it reachable was granted only in `src/tests/Rules.mk`. So the DB3 tests
compiled and `aimee-kb` did not. `aimee-kb` also never linked
`modules/db2/db3_route.o`, which `pgvec_db3.c` calls into, and seven test
fixtures linked the transport without the boundary it now calls.

Nothing caught this because a tree that does not build is not a test that fails.

### pgvec_code_search was declared with nothing behind it

The commit that renamed it to `pgvec_code_search_pgvector` never landed the
wrapper meant to take its name. The declaration stayed in the header, no
production caller and no test referenced the symbol, and so the link stayed
quiet while the commit message and the portability audit both recorded that the
code search routes. It answered from pgvector every time.

### The externalization end-to-end had never passed

`TestDB3GoProvidersOperateOverAuthenticatedCBus` is the only proof that DB2's
router reaches a real Go provider over the real bus rather than each half being
tested against a fake of the other. It attached its providers as principal refs
1001 and 1002, which stopped being valid when the reserved provider band
`[456, 512)` was introduced: `ValidateProviderRef` refused them inside
`ObserveCapabilities`, no route ever deployed, and the failure surfaced as a
route query that never came good.

It reproduces on `testing`, so it was not this work's doing. It went unseen
because nothing runs it. `scripts/test_db3_go_bus.sh` was named in no CI job and
no make target, and `scripts/check_tests_are_run.py`, the check written for
exactly this class, only matches targets named `unit-test-*`.

## The equivalence, checked on the engine

`code_verify.sql` builds the three cases where an equivalence argument usually
breaks and asserts the two forms agree on all of them: a retired generation
beneath a current one, a detached project, and a vector whose `projects` row is
gone. It also asserts that a detached and an unknown project each resolve to no
generation at all, which is what makes the wrapper decline to route rather than
route without the lifecycle condition, and that pgvector orders the routed
candidate set nearest first.

All of it passes on PostgreSQL 17 with pgvector 0.8.0.

The equivalent check for `kb_embeddings` was run by an earlier session and is
not reproduced here: the filter form and the join form returned identical rows,
excluding both a stale generation and an orphan, and re-applying the schema over
a database that predated the `generation` column added it, backfilled it from
`kb_documents`, and was idempotent.

## The write half, which had nothing at all

Everything above is about the READ side. The routed search only answers
correctly if the embedding row carries the right generation in the first place,
and nothing checked that it does: the one test touching `pgvec_kb_upsert`
asserts it returns 0 or -1 and does not crash, and never looks at the row.

That gap fails silently in the worst way. A NULL generation matches no filter,
so a routed kb search returns nothing and reads as a corpus with no hits rather
than a broken write.

`unit-test-pgvec-generation-pg` closes it, driven by `run-generation-test.sh` on
its own database. Against PostgreSQL 17 with pgvector 0.8.0, at the 384
dimensions the schema was applied at, for both `kb_embeddings` and
`kb_pdf_embeddings`:

- the generation that lands is the document's, through a statement that binds
  `:point_id` twice and relies on the parameter rewriter mapping a repeated name
  to one `$N`
- re-upserting after the document moves to a new generation carries the new
  value through `ON CONFLICT`
- a point with no `kb_documents` row takes NULL rather than failing or
  defaulting

Its first run failed, and the fault was the test rather than the code: it used a
four-float vector, and the vector column's width is fixed when the schema is
APPLIED. Worth recording, because a fixture that picks its own dimension will
fail this way against any real database.

## Traps this harness records

Two of them cost real time, and both produce failures that look like code
defects and are not.

**Do not kill a run.** `make unit-tests` points `HOME` and `TMPDIR` at one
temporary directory and removes it from an `EXIT` trap. Killing the run fires
that trap while test binaries are still inside it, and they fail on `getcwd()`
with nothing in their output to explain why. One interrupted run produced 462
failures and 192 `getcwd` errors on a tree whose previous clean run had passed
every binary.

**Delete zero-byte binaries after an interrupted run.** An interrupted link
leaves an empty output file that make considers up to date, so the next run
executes an empty file and reports a failure that is entirely self-inflicted.
`find src/build -type f -size 0 -delete` before trusting anything.

**Do not run the two scripts at once.** They share one build tree, and two
parallel makes in it corrupt each other's objects.

## What is still not proven here

**No test drives the whole path.** Ingest, embed, then a routed search returning
correct rows from a real external provider over real pgvector data is still
inferred from parts rather than observed, because each half is tested against a
fake of the other: `routed_test.go` has a real router and a real provider but a
stub pgvector fallback, `TestDB3GoProvidersOperateOverAuthenticatedCBus` has a
real bus and real providers but a fake search function, and `code_verify.sql`
has real pgvector but is SQL rather than the C that generates it. Closing this
means standing aimee-kb up with a provisioned provider, the shape
`docs/validation/aimee-module-on-a-clean-container.md` already uses.

The `unit-test-agent` failure seen on the developer's machine does not reproduce
in the container, so it is environmental rather than a defect, but its cause is
not identified.

`scripts/test_bus_conformance.sh` remains broken on `testing` at its
module-runtime stage for `config`, `execution-policy`, `postgres`, and `aimee`.
That script and `scripts/check_bus_perf_gate.sh` are run by no CI job and no
make target. Neither was changed by this work, and neither is fixed by it.
