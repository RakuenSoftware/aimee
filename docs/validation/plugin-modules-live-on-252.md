# Plugin modules: live validation on 192.168.1.252

- **Date**: 2026-08-21
- **Host**: Proxmox `pvetest` at 192.168.1.252.
- **Reproduce**: `sh scripts/plugin-full-e2e-252.sh` (server leg),
  `CT=<vmid> sh scripts/plugin-full-e2e-252.sh` (both legs).
- **Scope discipline**: the server leg lives under `/tmp/al2`, the kb leg under
  `/opt/plive` plus the scratch database `aimee_plive`; both are removed at the
  end and teardown is scoped to them, never a blanket `pkill`. `.252` hosts live
  deployments in **other** LXC containers and nothing here touches them.

## Why there are two legs

A real `aimee-kb` needs a real PostgreSQL, so it runs inside an LXC container
with its own postgres. A real `aimee-server` needs neither and runs on the host.

The split matters for more than convenience. **Only the kb leg attaches the real
`postgres` module**, and that is the configuration that exposed the defect below.
A scratch host running no other modules cannot see it, which is exactly why the
earlier single-daemon validation passed while the bug was present.

## What this run found

### 1. The plugin event-kind range overlapped postgres, db2 and db1

The headline defect, and the reason to run against a real kb at all.

`docs/modules/README.md` states the rule: a module's event kinds are carved from
its principal ref as **`4096 + ref*256 + stage`**. A ref therefore reserves a
whole 256-kind block, whether or not it uses every stage.

Plugin instances did not follow that rule. They drew from a *separate* range
starting at 11264, chosen (per the comment in `mcp.go`) as "the next free
256-aligned block after the highest currently allocated kind (11010, block 43)".
That reasoning inspected the highest kind actually *in use* rather than the
blocks the rule *reserves*. And `4096 + 28*256 = 11264` is precisely postgres's
block. The range squatted:

| Canonical module | ref | block | plugin range overlap |
|---|---|---|---|
| postgres | 28 | 11264–11519 | yes (serves 11265) |
| db2 | 29 | 11520–11775 | yes (serves 11521) |
| db1 | 30 | 11776–12031 | yes (serves 11777–11784) |

Reproduced live: with the real postgres module attached to a real `aimee-kb`,
the plugin instance was **refused at attach**,
`mcp-coll attach: bus: attach denied`. `bus_host_serve_kind()` binds one kind to
exactly one serving slot, so whichever attaches second loses. Postgres attached
first here; **in the other order the plugin would have denied postgres**, taking
`db2_ok` down with it, and the only trace would have been in the module's own log.

The provisioner had partially hidden this: `taken()` reads the `serve=` lines of
other grants in the same policy dir, so it *avoided* kinds it could see. It
cannot see a module that is not deployed yet, is granted in a different
directory, or is enabled later, and the failure then appears at attach time, far
from the provisioning that caused it.

**Fix.** The principal ref is now the single allocation authority, and kinds are
derived from it by the canonical rule. Two instances can only share a kind if
they share a ref, which the provisioner already prevents. Concretely:

- `EventPair(base)` → `EventKinds(ref)` in `server-go/modules/mcp`.
- `AIMEE_MODULE_EVENT_BASE` is retired. An instance still carrying one that
  disagrees with the derivation is **refused with a message telling the operator
  to re-provision**, silently ignoring it would leave a grant naming postgres's
  kinds.
- The provisioner rewrites an old grant's `serve=` list from the derivation.
- Refs 200–455 are reserved for plugin instances in
  `tests/baselines/modules/canonical-inventory.yaml`, and
  `scripts/check_module_inventory.py` now **fails** if any module is assigned a
  ref inside that band (it also now checks ref uniqueness and that a retired ref
  is not still assigned, none of which was validated before).

Post-fix, ref 200 derives kinds 55297/55298, clear of every canonical block, and
a plugin instance and the postgres module coexist on one kb bus with
`db2_ok` still true.

### 2. `install: kb` is not a plugin-module concept

An earlier version of this document listed "`install: kb`, plugin modules hosted
by `aimee-kb`" as untested; that was wrong twice over; `install: server|kb` is a
field of the **existing `aimee.yaml` `mcp_clients`** path, not of plugin modules;
`aimee-kb` boots its share via `mcp_client_registry_boot(CONFIG_MCP_INSTALL_KB)`.

And plugin modules hosted by kb are **not implemented**, which is a different thing from untested.
Every caller of `aimee_module_commands_*` is in `src/server/`, and `aimee-kb` has
no `/v1/commands`, no CLI manifest and no MCP tools route. A plugin module can
attach to kb's bus (it did, above) but nothing collects or exposes its commands.
Plugin modules are aimee-server-hosted by design; the kb leg exercises kb as the
**host of the bus and of `install:kb` clients**, which is what it actually is.

### 3. Earlier operational findings, still true

- **Grants load once, at daemon start.** `bus_runtime_start()` reads the policy
  directory at boot, so an instance provisioned while the daemon runs is denied
  at attach until it restarts, and the denial appears only in the module's own
  log. Provision before starting, or restart after provisioning.
- **The operator surface refreshed nothing.** `GET /v1/dashboard/metrics` read the
  plugin status table without triggering a collect, so "is my plugin running"
  answered "no plugins" while an instance was attached and serving. It now
  refreshes on the same TTL as the other surfaces. Only a live server exposed it.
- **UNIX socket paths silently truncate.** Running from the agent worktree
  produced a socket literally named `a`: the path was ~118 bytes against the
  108-byte `sun_path` cap, so it truncated, the server came up, and its socket
  was unreachable with nothing logged. Not caused by this work and not fixed
  here, but a truncated bind should fail loudly.
- **A link regression.** Extracting the OSV gate into `mcp_osv_gate.c` broke the
  `aimee-kb` link; `make -j8` builds the default `all` target and does **not**
  build `server`/`kb`, so earlier "clean build" claims were narrower than they
  sounded.

## Assertions

### Server leg: `scripts/plugin-full-e2e-server-remote.sh`, on the host

| Assertion | Result |
|---|---|
| Every grant derives its kinds from its `principal_ref` | pass |
| `aimee-server` `/v1` socket is up | pass |
| An **SSE** instance advertised its tool | pass |
| An **SSE-transport plugin answered a real dispatch over HTTP** | pass |
| Arguments reached the SSE plugin | pass |
| A **wrong bearer** did not authenticate | pass |
| 40/40 **concurrent** dispatches answered | pass |
| Metrics reports `principal_ref` (and no longer `event_base`) | pass |
| Metrics reports *this* instance's ref | pass |
| `tools_list` reports a registry generation | pass |
| A dead plugin's command was withdrawn | pass |
| The generation moved (9 → 14), so `listChanged` fires | pass |
| The module still answers after its plugin died | pass |
| The daemon came back up after a restart | pass |
| Instances do **not** re-attach on their own | recorded (known gap) |
| Processes left from the run | 0 |

### KB leg: `scripts/plugin-full-e2e-kb-remote.sh`, in a container

Run in a scratch LXC against PostgreSQL 17 + pgvector with a real db2 schema,
built from the debian-13 template by `scripts/plugin-e2e-ctprep.sh` and destroyed
afterwards. The whole leg reports **FULL E2E PASSED** with 0 leftover processes.

| Assertion | Result |
|---|---|
| Every plugin grant derives its kinds from its `principal_ref` | pass |
| postgres still owns 11265 (the kind the old range squatted) | pass |
| **`aimee-kb` came up against a real PostgreSQL** | pass |
| **kb reports `db2_ok=true` through the postgres module on the bus** | pass |
| **A plugin instance and the postgres module COEXIST on one kb bus** | pass |
| **postgres was not displaced: `db2_ok` still true** | pass |
| kb started the `install:kb` client | pass |
| kb did **not** start the `install:server` client | pass |
| `aimee-server` up on the same box | pass |
| Manifest advertises both instances and the full invoke path | pass |
| Dispatch crossed the bus to the real plugin, with arguments | pass |
| Instance 2 answered from ITS plugin | pass |
| A pluggy hook crossed the event bus to the real plugin | pass |
| server booted `install:server` and **not** `install:kb` | pass |
| A **hanging** plugin was bounded by the invoke deadline (30s) | pass |
| The hung instance did not take the others down | pass |
| Unknown verb / unknown group refused, not dispatched | pass |
| Non-JSON body and a 2MB body did not kill the daemon | pass |
| An **ungranted** instance is denied at attach | pass |
| An instance carrying the retired `AIMEE_MODULE_EVENT_BASE` is refused | pass |
| 300/300 soak dispatches, **0** fd growth | pass |
| 40/40 concurrent dispatches | pass |
| A dead plugin's command withdrawn; generation moved (79 → 87) | pass |
| **kb still reports `db2_ok=true` at the end of the run** | pass |
| Instances do **not** re-attach after a daemon restart | recorded (known gap) |
| Processes left from the run | 0 |

**On the container.** The first kb run used LXC 101 (`aimee-full-e2e-101`), which
another actor on the host stopped and destroyed partway through, `vzstop` then
`vzdestroy` by `root@pam` in the Proxmox task log, while the run was in progress.
The leg was re-run start to finish on a freshly created scratch container, so
every kb assertion above comes from one complete green run, not a salvaged one.
Something else on that host creates and destroys containers; pick a VMID clear of
it.

**One unexplained intermittent.** Across six runs of the concurrency step, five
returned 40/40 and one returned **36/40**. The bad run happened while a container
was being provisioned (apt install) on the same host, so heavy load is the
obvious suspect, but that is a guess: the diagnostic that dumps what a failed
dispatch actually returned was added afterwards and has not reproduced it in
three subsequent runs. It is recorded here as open rather than dismissed as
flake.

## On transports, because the tables above are easy to misread

HTTP is only how the test client reaches the daemon. The hop that carries a
command to its plugin is the **event bus**:

```
POST /v1/commands/<group>.<verb>        HTTP — ingress to the daemon only
  -> rh_command_invoke -> registry lookup -> plugin_command_invoke
  -> obs_bus_module_call(invoke_kind, STAGE_INVOKE)      THE EVENT BUS
  -> Go module handleInvoke -> client.CallTool
  -> the plugin's stdio (or, for an SSE instance, its HTTP endpoint)
```

Nothing between the daemon and a module is HTTP, and nothing here adds a second
transport there. The same is true of pluggy: its host is an MCP server behind a
Go module, reached over the bus like any other.

The MCP profile is `full` for these runs on purpose. Plugin tools are registered
`MCPDiscoverable` by design, `mcp_tool_profile.c` records that the prominent
list is a per-session tax on every client, and ~15 plugin modules would grow it
without bound, so the default `core` profile correctly filters them out and they
are reached via `find_tools`/`describe_tool`/`call_tool`. The full profile is
what proves the group tool is actually built and appended.

## Two harness bugs, recorded so they are not mistaken for product findings

- A bare `wait` in the soak step waits for **every** background job of the shell,
  which included the SSE server, the module instances and the daemons themselves,
  so it never returned. The runs now collect the curl PIDs and wait on those.
- The withdrawal and operator-surface assertions originally ran **after** the
  daemon restart, where no instance is attached (see the re-attach gap), so there
  was no plugin row and no generation to compare. They now run while instances
  are attached; the restart check runs last.

Also from the earlier round: an SSE instance's argv is `["sse:URL","ENV_NAME"]`,
not a bare `sse:URL` string; and one run's failures were leftover state from a
previous run that had been aborted mid-flight, not a regression.

## What this does and does not settle for the retirement

**Settled:** the plugin path works end to end on a real daemon, provisioning,
admission, registration, dispatch, withdrawal, the operator surface, both
transports, it coexists with the `aimee.yaml` path on the same server, and it no
longer collides with core module kinds.

**Not settled:**
- **No migration exists.** An `aimee.yaml` entry is one config line; a plugin
  module is a provisioned instance with a grant, a principal ref, derived kinds
  and its own process. Retiring without a converter strands every deployment.
- **The shared OSV gate still lives in `modules/protocols/mcp/`** and has to move
  before that directory can shrink.
- **No automatic re-attach.** Not implemented: an instance whose daemon restarts
  stays gone until it is restarted too. Confirmed live, recorded as a gap.
- **No long soak.** 300 sequential and 40 concurrent dispatches with no fd
  growth; nothing has run for hours or under sustained load.
- **No real MCP client.** The `tools/list_changed` watcher is unit-tested against
  the real thread (mutation-verified) and the generation is observed to move
  live, but no actual client such as Claude Code has been driven through a plugin
  appearing mid-session.
- **The kb leg's last four assertions** have not been re-run since the container
  was destroyed (see the caveat above).
