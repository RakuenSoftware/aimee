# Validating the DB1 module migration on a machine that never built it

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
  `aimee-module-db1` into `/usr/local/libexec/aimee-modules/` -- the path the
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
  attaches or during a workload -- read from `/proc/<pid>/fd`, so it is a
  property of the process, not of the code that was read
- with nothing serving the store, health says `unavailable` and a session create
  **fails** rather than fabricating, and the daemon survives the attempt
- the module creates the store -- **102 tables** -- and is the process holding it
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
checks passed, covering every family's shapes across the real bus -- nested
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
  **0** on the same store -- the migration's central claim, measured across the
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

Every script above runs two processes: the daemon and the module. The container
runs **three**. `server-entrypoint.sh` defaults `AIMEE_WFE_ENGINE=go` and
launches `aimee-wfe` with `--home` and no `--db`, so `cmd/aimee-server` falls
back to `$home/aimee.db` -- the module's file -- and opens it with `sql.Open`.
The bundle generates a `wfe.grant` for it (principal_ref 64), and that grant
requests kinds 6657, 6678 and 9474 and **no DB1 kinds at all**: the Go WFE does
not reach the store through the module, it reaches it through the filesystem.

Run with all three up, on a clean container:

- **two processes hold `aimee.db`** -- `aimee-module-db` and `aimee-wfe`, read
  from `/proc/*/fd`. The module is not the store's sole owner in the shipped
  configuration, and that is the migration's central claim not holding for the
  appliance
- the Go side **amends the module's schema**: 102 tables to 105, and five
  columns added to `lifecycle_work_item` (`source_path`, `reserved_cost_usd`,
  `reservation_state`, `reservation_owner`, `reservation_lease_until`). The C
  module references none of the five, so it is additive, not conflicting
- it does this **even when it cannot run**: the first attempt had no `wfe.grant`
  installed, so the WFE died on `bus: attach denied` -- after opening the store
  and running its migrations
- **either process will create the store**: started first on an empty home the
  Go WFE creates seven tables on its own, and the module then completes the
  schema to the same 105. Both orders work, and `lifecycle_work_item` ends with
  the same column set in a different column order -- two authorities, and which
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
Four external writers insert into `lifecycle_work_item` -- a Go-owned table --
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
gone partway through, with the Proxmox host itself up for 2 days -- so not a
reboot. Something else manages this host.

**A foreign binary was installed into the container mid-run.** A re-run of the
e2e script suddenly reported 17 passed / 6 failed, including "daemon created a
database on its own" and "daemon holds 3 open database descriptors" -- the
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
died, `/v1/server/health` kept reporting `"state":"ok"` for ~37s -- 30s of bus
heartbeat staleness plus up to 7.5s of reap interval -- while every store call
was already failing. Health was reading module availability, which is registry
state, and the registry is only corrected by the reaper.

It is now probed rather than inferred: `db1_store_probe()` asks the store a real
question and caches the verdict for a second, and only the health handler uses
it -- `db1_store_ready()` stays the cheap predicate in front of every
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
the product's -- `/v1/sessions/list` is a POST route, create answers with
`session_id` rather than `id`, the table is `server_sessions`, and asserting
readiness the instant the module's socket appears races the daemon's dial. They
are noted because a validation script that is wrong in the product's favour is
worth more suspicion than one that is wrong against it, and two of these were.

## Reproducing

Install the three binaries as above, then inside the container:

    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-e2e.sh
    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-explore.sh
    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-readiness-probe.sh
    AIMEE_DB1_GRANT=/path/to/db1.grant scripts/validation/db1-module-write-contention.sh

The coexistence script additionally needs `aimee-wfe` and the WFE grant, and is
expected to fail until the Go WFE stops opening the store:

    AIMEE_DB1_GRANT=/path/to/db1.grant AIMEE_WFE_GRANT=/path/to/wfe.grant \
      scripts/validation/db1-module-wfe-coexistence.sh

The upgrade script additionally needs a pre-migration build:

    OLD_SERVER=/opt/old/aimee-server OLD_MODULE=/opt/old/aimee-module-db1 \
      scripts/validation/db1-module-upgrade.sh

`AIMEE_DB1_MODULE` overrides the module path for running against a build tree.
