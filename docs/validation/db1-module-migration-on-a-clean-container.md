# Validating the DB1 module migration on a machine that never built it

The repo's own suites run against a build tree, with the module started by the
harness beside its socket. That is the right shape for CI and the wrong shape
for answering "does the thing we ship work". This is the record of running the
migrated build on a machine that had never seen the source.

## What was used

- **Host**: Proxmox `pvetest` at 192.168.1.252, `pve-manager/9.2.6`.
- **Container**: LXC **CT 9010** `aimee-db1-verify`, created for this, Debian 13
  standard template, 4 cores / 6GB / 24GB disk, unprivileged, DHCP.
  CT 9001 (`db2-replay`) and CT 9002 (`aimee-e2e`) belong to other work and were
  not touched.
- **Build**: `feature/db1-module-migration` at `5a86a608c5`, built locally,
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

## What it turned up

**One finding in the product**, written up as
`docs/proposals/pending/db1-health-is-a-heartbeat-not-a-probe.md`: after the
module dies, `/v1/server/health` keeps reporting `"state":"ok"` for ~37s (30s
heartbeat staleness plus up to 7.5s of reap interval) while every store call
fails. Bounded, not a latch, and not introduced by the migration -- but it is
the health endpoint disagreeing with the daemon's own behaviour during the part
of an outage when someone is looking.

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

`AIMEE_DB1_MODULE` overrides the module path for running against a build tree.
