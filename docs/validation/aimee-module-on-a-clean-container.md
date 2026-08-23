# Validating the aimee module on machines that never built it

The module's own suites call `Handle` in process. That is the right shape for a
unit test and the wrong shape for answering "does the thing we ship run at all".
This is the record of installing the `aimee` module on containers that had never
seen the source, bringing up aimee-server and aimee-kb, and driving the module
over a real event bus.

It found two defects that no unit test could have found, both of which would
have shipped.

## What was used

- **Host**: Proxmox `pvetest` at 192.168.1.252, `pve-manager/9.2.6`.
- **Containers**, both created for this and both Debian 13 standard, unprivileged, DHCP:
  - **CT 9090** `aimee-peer-verify`, 4 cores / 4GB / 16GB, at 192.168.0.84 -- aimee-server.
  - **CT 9091** `aimee-kb-verify`, 4 cores / 6GB / 20GB, at 192.168.0.97 -- aimee-kb.
- **Build**: this worktree at `pre-merge-safety-2831-ga2fac47caa` plus the peer-messaging
  work, built locally and carried in as binaries. Nothing was compiled on the
  Proxmox host or in either container.
- **Installed**: `aimee`, `aimee-server`, `aimee-kb` into `/usr/local/bin`; the Go
  multicall runtime under each attested `aimee-module-<id>` name; the one C module
  per placement (`aimee-module-db1` on the server, `aimee-module-db2` on the kb);
  the real `aimee-module-config`, which is a separate program. Grants came from
  `scripts/export_c_repositories.py --runtime-bundle`, unmodified.

## What it found

### 1. The module was declared, green in every test, and would never have run

`export_c_repositories.py:1056` computes

```
enabled = module_id in required or descriptor.get("enabled_by_default") is True
```

and only enabled modules reach `server.modules`, which is the manifest the module
supervisor spawns from. The `aimee` descriptor had `enabled_by_default: false`
and the module was listed **optional** in the canonical inventory, so it was
absent from `server.modules` entirely.

Every unit test passed. Every module validator passed. The module would have
been installed, granted, and never started -- and nothing in the repository
would have said so, because "declared" and "spawned" are different facts and
only one of them is checked.

The first fix was `enabled_by_default: true`, matching the other
optional-but-running modules. `validate_module_descriptors` then refused it: an
optional module may only be default-on if it is in the validator's `DEFAULT_ON`
set, which also forces `runtime_toggle.supported: true` -- a live enable/disable
this module must not have, because a session waiting on a peer would have no
defined outcome if delivery vanished underneath it.

So `aimee` is **required** instead, which is what it should have been: a server
without the home for aimee-server-specific functionality is not a smaller
server. That took three more declarations, each of which failed loudly rather
than silently -- `REQUIRED_COUNT`/`OPTIONAL_COUNT` in `check_module_inventory.py`,
and `PROCESS_REQUIRED` in `validate_module_process_contracts.py`. `aimee` then
appears as entry 14 in `server.modules`.

The general shape is worth naming: "declared", "enabled", "required" and
"spawned" are four different facts about a module, and only the first was
checked by anything the module's own tests ran.

### 2. One unresolvable grant refuses every module's admission

`parse_grant_file()` resolves the `executable` line with `realpath()`, and
`bus_runtime_policy_load_dir()` rejects the **whole directory** if any single
entry fails to parse. The server logged one line:

```
ERROR obs_bus: module grant policy is invalid: /var/lib/aimee/modules.d/server
```

naming the directory and not the offending file. The cause was three grants
whose pinned binaries this container does not install: `wfe` and `workflows`
(both pin `/usr/local/bin/aimee-wfe`, an externally hosted process) and `db1`
(the one C module on the server placement, not yet built at that point).

This is a deployment-shaped failure with a deployment-shaped blast radius: a
grant for a component you did not install stops **every** module being admitted,
including ones that are present and correct.

Handled here by seeding only grants whose executable resolves and **reporting
what was skipped**, since a skipped grant is a component the run did not
exercise. A production image installs every executable it writes a grant for.

## What the module was proved to do

`aimee-peerprobe` attaches to the live bus as an admitted client (principal
1/**69**, granted `request=` this module's kinds) and drives all four stages.

That ref is deliberately not 67. 67 is reserved for the module's own OUTBOUND
identity (`aimee-db1`, for reading the session directory out of db1), and the
probe held it at first — harmless only because that client does not exist yet.
Once `DirectorySource` is wired the two would be a duplicate principal and the
bus would refuse whichever attached second, so the failure would surface long
after the cause. 69 is validation-only and is not declared in
`process-contracts.json`; its grant is written by hand into the container and
never shipped.

Fourteen checks, green on consecutive runs:

| Check | Result |
|---|---|
| delivery stage 12033 reachable | pass |
| unknown sender refused as a SERVED call | pass |
| inbox stage 12034 reachable | pass |
| unknown session refused, not reported as an empty inbox | pass |
| grant stage 12035 reachable | pass |
| absent grant answers OK with a false value | pass |
| grant write accepted | pass |
| grant readable back (the module holds state) | pass |
| grant did not leak in the reverse direction | pass |
| malformed frame refused at the TRANSPORT level | pass |
| channel stage reachable | pass |
| channel send by an unknown sender refused | pass |
| members of an absent channel answers OK with none | pass |
| unadvertised stage not served | pass |

The load-bearing result is the pair in the middle. A domain refusal
(`unknown_sender`, `no_peer`) arrives as a **successful** bus call carrying a
status in the body, while a malformed frame arrives as a transport error
(`status 4`). That three-way distinction is what lets a governance tap tell "the
module is broken" from "the module said no", and it could not be tested in
process because in process there is no transport to distinguish it from.

`aimee-module-aimee` is a supervised long-lived process holding its own state:
the grant written by one probe invocation is still there for the next.

## Both services up

- **aimee-server** (CT 9090): 14 module processes attached including `aimee`;
  `/v1` on 127.0.0.1:8740 answering 401 without a bearer, which is the listener
  live and authenticating.
- **aimee-kb** (CT 9091): `kb_http listening on 127.0.0.1:8790`, `/v1/health`
  200, db2 pool initialized against PostgreSQL 17 + pgvector 0.8.0, ingest
  workers and the curator index lane started.

## What the evidence covers

The run was captured at commit `ffb60ac`. A later commit (`c597394`) split the
peer wire into its own package so the module can rebase onto the db1 absorption
without a name collision. That restructuring changed no stage id, no event kind,
no grant, and no wire byte, and the contract tests that pin all four still pass,
so the run above still describes what ships.

Stating it rather than assuming it: a refactor after the evidence was taken is
exactly the kind of thing that quietly makes a recorded result describe a tree
that no longer exists.

## Teardown

Both containers were destroyed and their staging files removed when the run
finished, per the host's own rule in `/root/AGENTS.md`: a resource created to
test something is cleaned up before the task is complete, and a successful test
does not waive that. Verified after the fact — `pct status` reports no config
for either id, the thin-pool space is back, and every guest belonging to another
session is untouched.

Two things worth carrying to the next run of this, because both were learned the
hard way here. The teardown should be arranged BEFORE the test rather than
remembered after, so it also runs on failure or interruption. And the host runs
a reaper on two independent clocks — a 4h lease renewed by `aimee-keepalive
ct:<id>`, and a 4h liveness clock that no renewal resets, so an idle guest dies
however recently it was leased. A guest vanishing mid-run is that working
correctly, not the host being unreliable.

Rebuilding is cheap and the record above says exactly what to install, which is
the right trade: a container kept alive across hours of unrelated change is no
longer the clean room the evidence claims it was.

## What this run did NOT exercise, stated so the record is not read as more than it is

- **Peer messaging end to end between two sessions.** The module has no
  `DirectorySource` wired, so sessions cannot register over the bus yet and the
  probe drives refusal paths rather than a delivered message. The send/reply path
  is covered only by the in-process suite.
- **Retrieval quality on the kb.** The embedder is a stub returning hash-derived
  vectors with no semantic content. It proves the service starts and the wire
  works; any relevance measured against it is noise.
- **The in-image PostgreSQL.** The kb was pointed at the container's stock
  cluster via `AIMEE_DB2_URL` because the internal one refuses to run as root,
  which is PostgreSQL's rule rather than aimee's.
- **`workflows` and `wfe`**, whose grants were skipped, and the LLM synthesis
  lane, which has no endpoint configured.
- **The ref 30 absorption.** This run validated the module at principal ref 31,
  stages 1/2/3, kinds 12033/12034/12035. The agreed end state folds peer
  messaging into ref 30 as stages 20/21/22 (kinds 11796/11797/11798). The
  mechanism is proved; those numbers are not final and the run must be repeated
  after the renumber.
