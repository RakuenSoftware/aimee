# Validating the DB1 module migration on a machine that never built it

> **HISTORICAL. THIS VALIDATED A PROGRAM THAT NO LONGER EXISTS.**
>
> Superseded, and recorded rather than deleted because the method still reads
> well and the failures it found were real. What it does NOT do is say anything
> about the store module as it now stands.
>
> - It ran `feature/db1-module-migration` at `0e44f5a08d`. The store was a C
>   module over **SQLite**; it is now a Go module over PostgreSQL that opens no
>   database at all and reaches the postgres module over the bus.
> - Every count below (23 checks, 43 commands, 102 tables) is from that run.
>   The scripts it names have since been rewritten and now refuse to start
>   without `AIMEE_STORE_URL`; `db1-module-e2e.sh` today makes 12 checks, not 23,
>   and `db1-module-upgrade.sh` no longer exists.
> - CT 9077 was destroyed under the host's cleanup rule, so nothing here can be
>   re-checked against the run that produced it.
>
> A validation record is the one artefact in a tree with no build to fail it,
> which is what lets its subject be replaced underneath it while it goes on
> reading as current. Anything below that sounds like a claim about today is not
> one. `scripts/check-validation-record.py` compares a record's asserted check
> counts against the scripts that produce them; this file is exempt from it by
> the marker above, which is the assertion that it is history.

The repo's own suites run against a build tree, with the module started by the
harness beside its socket. That is the right shape for CI and the wrong shape
for answering "does the thing we ship work". This is the record of running the
migrated build on a machine that had never seen the source.

## What was used

- **Host**: Proxmox `pvetest` at 192.168.1.252, `pve-manager/9.2.6`.
- **Container**: LXC **CT 9077** `aimee-db1-verify-2`, created for this, Debian
  13 standard template, 4 cores / 6GB / 24GB disk, unprivileged, DHCP. An
  earlier CT 9010 was used for the first passes; it is described under "the host
  is shared" below, and the final numbers here are all from 9077.
- **Build**: `feature/db1-module-migration` at `0e44f5a08d`
  (`pre-merge-safety-2544-g0e44f5a08d`, printed by the deploy and by the run so
  every result names the binary that produced it), built locally,
  carried in as binaries. Nothing was compiled on the host or in the container,
  and nothing was installed on the Proxmox host itself.
- **Installed**: `aimee` and `aimee-server` into `/usr/local/bin`,
  `aimee-module-db1` into `/usr/local/libexec/aimee-modules/`. The path the
  image build uses. The grant came from
  `scripts/export_c_repositories.py --runtime-bundle`, unmodified, so the served
  kinds are the ones the build declares: 11777-11795, nineteen families.

The container needed no library it did not already have: the server wants
libsqlite3, libssl/libcrypto, libz, libzstd and libpam, and the module wants
libsqlite3 alone. `sqlite3` and `curl` were installed inside the container as
test instruments, not as runtime dependencies.

## What was checked, and what it found

### `scripts/validation/db1-module-e2e.sh` -- 23 checks, all passing

The claims the migration makes, asserted against an installed deployment and an
empty home:

- the daemon starts with no database present and **creates none**
- the daemon holds **no descriptor on any `.db`**, before or after the module
  attaches or during a workload. Read from `/proc/<pid>/fd`, so it is a
  property of the process, not of the code that was read
- with nothing serving the store, health says `unavailable` and a session create
  **fails** rather than fabricating, and the daemon survives the attempt
- the module creates the store (**102 tables**) and is the process holding it
  open
- a session created over the API comes back from `/v1/sessions/list` **and** is
  on disk in `server_sessions` in the module's file
- the session is still there after the module is killed and restarted

### `scripts/validation/db1-module-explore.sh` -- 43 commands, nothing suspect

The CLI driven across surfaces reaching all nineteen families, watching for
`capability absent`, `no such table`, internal errors, a dead daemon or a dead
module. None appeared, the daemon never opened a database, and writes landed in
the module's store.

This is the weakest of the three: against an empty store many read commands
answer "none" without reaching the store at all, and several printed subcommand
usage rather than running. It is a smoke test for "nothing is grossly unwired",
not evidence that each family works.

### The repo's own module-bus suite, against the **installed** binary

`unit-test-db1-module-bus` takes the module path as its argument, so the built
test was carried in and pointed at `/usr/local/libexec/aimee-modules/`. All 24
checks passed, covering every family's shapes across the real bus, nested
rows, allocated lists, nine-parameter updates, refusals that are answers rather
than errors. This is the real cross-family evidence; the sweep above is not.

### `scripts/validation/db1-module-upgrade.sh` -- 11 checks, all passing

Everything above starts from an empty home, where the module creates the schema
itself. No deployment that matters is empty. This one builds the **pre-migration
daemon** from `origin/testing` (`ab13bd87ab`, twelve families declared, eight
served, `db1_init` still linked into the daemon), runs it against a fresh home,
writes sessions through it, stops it, and brings the migrated build up on the
same file.

- the old daemon held **3 open descriptors** on `aimee.db`; the new one holds
  **0** on the same store, the migration's central claim, measured across the
  upgrade rather than asserted about a fresh file
- the new build **starts** on a store it did not create
- **no table lost**: 102 before, 102 after
- every session the old build wrote is still on disk **and** reads back through
  the new build's API
- the upgraded store still takes writes, and the new row lists beside the old

One thing this turned up about the *old* build, recorded rather than fixed
because it is not this branch's: the pre-migration daemon reliably fails its
first store call after health has already gone green, then serves every call
after it. The migrated build's equivalent is asserted in the e2e script, whose
first store call comes directly after readiness and succeeds.

### `scripts/validation/db1-module-wfe-coexistence.sh` -- the topology that ships

**This section describes what the script found BEFORE the engine was moved onto
the module.** It is kept because it is the measurement that justified the work;
the same script passes now, and the closing note at the end of this document
records that. The finding, in the tense it was found in:

Every script above runs two processes: the daemon and the module. The container
runs **three**. `server-entrypoint.sh` defaults `AIMEE_WFE_ENGINE=go` and
launches `aimee-wfe` with `--home` and no `--db`, so `cmd/aimee-server` falls
back to `$home/aimee.db`, the module's file, and opens it with `sql.Open`.
The bundle generates a `wfe.grant` for it (principal_ref 64), and that grant
requests kinds 6657, 6678 and 9474 and **no DB1 kinds at all**: the Go WFE does
not reach the store through the module, it reaches it through the filesystem.

Run with all three up, on a clean container:

- **two processes hold `aimee.db`**: `aimee-module-db` and `aimee-wfe`, read
  from `/proc/*/fd`. The module is not the store's sole owner in the shipped
  configuration, and that is the migration's central claim not holding for the
  appliance
- the Go side **amends the module's schema**: 102 tables to 105, and five
  columns added to `lifecycle_work_item` (`source_path`, `reserved_cost_usd`,
  `reservation_state`, `reservation_owner`, `reservation_lease_until`). The C
  module references none of the five, so it is additive, not conflicting
- it does this **even when it cannot run**: the first attempt had no `wfe.grant`
  installed, so the WFE died on `bus: attach denied`, after opening the store
  and running its migrations
- **either process will create the store**: started first on an empty home the
  Go WFE creates seven tables on its own, and the module then completes the
  schema to the same 105. Both orders work, and `lifecycle_work_item` ends with
  the same column set in a different column order, two authorities, and which
  arrived first stays visible in the file

This is not corruption waiting to happen: both sides are configured for
multi-process access deliberately (module `journal_mode=WAL` with 5s/15s busy
timeouts; Go WAL, 5s busy timeout, `MaxOpenConns(1)`, `_txlock=immediate`).
`scripts/validation/db1-module-write-contention.sh` drives external writers
against `lifecycle_work_item` while the module takes writes through the daemon
and checks for lost rows, lock failures and `PRAGMA integrity_check`.

The finding is architectural and it is real: **the doctrine is satisfied in C
and not in the appliance.** It is tracked in
`docs/proposals/pending/db1-the-go-wfe-still-opens-the-file.md`, which now
carries these measurements in place of the reasoning it was written from.

### `scripts/validation/db1-module-write-contention.sh` -- 7 checks, all passing

Whether two owners is also an operational problem, not just a doctrinal one.
Four external writers insert into `lifecycle_work_item`. A Go-owned table --
while the module takes writes through the daemon. At 4x40 external rows against
40 module writes: **no write failed on either side, no row was lost, no
lock/busy complaint, `PRAGMA integrity_check` clean**, store healthy afterwards.
A single create measured 9ms while contention was running.

That is evidence about a rate, not a proof about all rates, and it is worth
being clear which: both sides configure WAL with busy timeouts precisely so this
works, so passing was the expected result. "We believe WAL handles it" and "we
watched it handle it at this rate" are different claims and this is the second.

## The host is shared

Two things happened during this work that are worth writing down, because both
would have produced confident and wrong conclusions:

**Containers disappeared.** CT 9001, 9002 and the 9010 created for this were all
gone partway through, with the Proxmox host itself up for 2 days, so not a
reboot. Something else manages this host.

**A foreign binary was installed into the container mid-run.** A re-run of the
e2e script suddenly reported 17 passed / 6 failed, including "daemon created a
database on its own" and "daemon holds 3 open database descriptors": the
migration's central claim failing outright. It was not failing:
`/usr/local/bin/aimee-server` had been replaced with
`pre-merge-safety-2546-gc8d90b60d5`, a build from another branch, timestamped
between two of my own deploys. Re-running the identical script against a
verified binary in an isolated container gave 23/0.

Both of those results looked like product findings and neither was. The fix is
cheap and now permanent: the deploy prints
`aimee-server --version` and the module's md5 before any suite runs, so every
result names the binary that produced it. A validation run that cannot say what
it tested is not evidence.

## What it turned up

**One finding in the product, found here and fixed here.** After the module
died, `/v1/server/health` kept reporting `"state":"ok"` for ~37s, 30s of bus
heartbeat staleness plus up to 7.5s of reap interval, while every store call
was already failing. Health was reading module availability, which is registry
state, and the registry is only corrected by the reaper.

It is now probed rather than inferred: `db1_store_probe()` asks the store a real
question and caches the verdict for a second, and only the health handler uses
it, `db1_store_ready()` stays the cheap predicate in front of every
store-backed command. **Measured on the same container by the same script: 1s.**
`test_integration.sh` kills the module and asserts health stops saying "ok"
within five seconds, so a regression to inferring fails by twenty seconds rather
than by a hair. Written up in
`docs/proposals/pending/db1-health-is-a-heartbeat-not-a-probe.md`, now RESOLVED.

**Two in the integration harness**, both fixed in `src/tests/test_integration.sh`:

- `install_db1_module` returned 0 when the module binary was missing, leaving a
  run where every store-backed check fails as "failed to create session". That
  was correct when the daemon had an in-process fallback and is not correct now.
  It aborts with an explanation instead.
- the fallback serve list, used when the generated grant is absent, still named
  the **eight** kinds from when the module served eight families. Eleven
  families would have been silently unserved. The harness now generates the
  grant rather than guessing at it.

Both were live: `src/build/obj` had been wiped by a version change, so the
module binary and the generated grant were genuinely absent at the point this
was found.

Several failures in the first runs were the validation scripts' own bugs, not
the product's, `/v1/sessions/list` is a POST route, create answers with
`session_id` rather than `id`, the table is `server_sessions`, and asserting
readiness the instant the module's socket appears races the daemon's dial. They
are noted because a validation script that is wrong in the product's favour is
worth more suspicion than one that is wrong against it, and two of these were.

## After the engine moved

The section above is the measurement that justified the work. The engine reaches
DB1 through the module now, and the same script (unchanged) is the check
that says so. What changed underneath it:

- `server-go/internal/db1` is a mapping onto module operations rather than a
  SQLite driver. Nothing in `server-go` calls `sql.Open` outside tests, and
  `--db` is gone from `aimee-wfe` because there is no path left to pass it.
- the module's schema is the only schema: 105 tables and the five
  `lifecycle_work_item` columns the Go side used to add, so a second writer has
  nothing left to create or alter.
- the Go client is generated from the same catalog as the C one, because
  hand-writing wire encoders is how a contract and its callers drift.
- the engine waits for the bus and then for the module to answer before it asks
  anything real. A crash-looping engine is a worse failure than a slow one, and
  the boot order is not something it controls.

Measured on CT 9077, server `pre-merge-safety-2554-g511e7e18bb`, with the Go WFE
running beside the module exactly as the container runs it:

    holders of aimee.db: aimee-module-db(6043)
    holders of aimee.db now: aimee-module-db(6043)
    tables: 105 -> 105
    13 passed, 0 failed, 0 noted

One holder before the engine starts and one after. The table count does not move,
because there is nothing left for the engine to create or alter. The e2e suite is
unchanged at 23/0.

The Go tests now run against a real module rather than a temp SQLite file, and
that is what caught the five behaviours the port had lost, reconcile answering
the wrong question, budget parks stranding a sibling's authorised money, resume
clearing pauses only the engine may clear, a lost create race reading as a
broken store, and a found retry detail reported as a miss. Each is named in the
commit that fixed it. Reading SQL out of a function is not the same as reading
the function.

### `scripts/validation/db1-module-wfe-lifecycle.sh` -- 16 checks, all passing

Every other script here proves something about the store: who holds it, what it
creates, whether it survives an upgrade. The coexistence script even starts the
engine, but only to see whether it opens the file. None of them drives the
engine's own API, so none of them proved the thing the port was actually for.

This one submits a real run over the deployed engine's socket and checks each
answer against the module's store read independently with sqlite3. The API
agreeing with itself is not evidence; the API agreeing with the file the module
owns is. It covers submit, list, fetch by id, event history, pause, resume and
stop, and re-checks after every mutation that the engine holds no descriptor and
the module is the only process that does.

Two things it made explicit that the unit tests could not:

- the engine's per-run access check resolves the owner by walking parent links
  **through the module**, so calling with an identity exercises that lookup as
  well as the operation.
- a park is guarded on `pause_reason=''`, so re-parking an already-parked run is
  refused and the original reason stands. With no runner configured the
  scheduler parks the run on its own, at a moment the test cannot control, so
  the assertion is derived from the outcome rather than from a pre-read: an
  acceptance must leave a reason behind, a refusal must find one already there,
  and the inconsistent pairs are what fail. Three consecutive runs: 16/0 each.

## Reproducing

Install the three binaries as above, then inside the container:

    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-e2e.sh
    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-explore.sh
    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-readiness-probe.sh
    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-write-contention.sh

The lifecycle script additionally needs `aimee-wfe`, the WFE grant and the
shipped workflow definitions:

    AIMEE_DB1_GRANT=/path/to/db1.grant AIMEE_WFE_GRANT=/path/to/wfe.grant \
      AIMEE_WORKFLOWS_DIR=/path/to/config/workflows \
      scripts/validation/db1-module-wfe-lifecycle.sh

The coexistence script additionally needs `aimee-wfe` and the WFE grant, and is
expected to fail until the Go WFE stops opening the store:

    AIMEE_DB1_GRANT=/path/to/db1.grant AIMEE_WFE_GRANT=/path/to/wfe.grant \
      scripts/validation/db1-module-wfe-coexistence.sh

The upgrade script additionally needs a pre-migration build:

    OLD_SERVER=/opt/old/aimee-server OLD_MODULE=/opt/old/aimee-module-db1 \
      scripts/validation/db1-module-upgrade.sh

`AIMEE_DB1_MODULE` overrides the module path for running against a build tree.
