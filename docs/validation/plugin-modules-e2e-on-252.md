# Plugin modules: end-to-end validation on 192.168.1.252

- **Date**: 2026-08-21
- **Host**: Proxmox `pvetest` at 192.168.1.252, x86_64, glibc 2.41, Python 3.13.5, 8 cores.
- **Under test**: `server-go/modules/mcp` (Go plugin module), the instanced dispatch in
  `server-go/cmd/aimee-module`, and the C command-registry driver
  (`src/module_commands.c`, `aimee_command_unregister_module`).
- **Scope discipline**: everything ran under `/tmp/aimee-plugin-e2e`, removed afterwards.
  `.252` runs a live aimee deployment of its own; no blanket `pkill`, nothing touched
  outside this run's directory. Leftover-process check after the run reported none.

## What was exercised

Two suites, both over **real processes and the real bus** — no mocks at the seams that
matter. Run twice each to check stability.

`unit-test-bus-plugin-process` — the single-instance vertical:

| Assertion | Result |
|---|---|
| Three instances + one caller admitted to a real bus runtime | pass |
| Instance declares its plugin's tools, names folded to the registry grammar | pass |
| A second instance declares under its own group over its own event kinds | pass |
| An instance with no plugin declares 0 and answers OK | pass |
| Invoke reaches the plugin under its ORIGINAL name and echoes arguments | pass |
| An undeclared verb is refused without reaching the plugin | pass |
| A length-mismatched invoke frame is refused rather than read past | pass |
| A dead plugin's commands are withdrawn; its module keeps answering | pass |
| Instances are admitted by the OSV gate before anything is spawned | pass |
| A **refused** plugin never executes (observed via a sentinel file) | pass |
| A real **pluggy** plugin declares and is invoked through the UNCHANGED MCP module | pass |

`unit-test-bus-plugin-scale` — the deployment's target shape and the ugly cases:

| Assertion | Result |
|---|---|
| 10 instances admitted by the gate before any plugin starts | pass |
| 10 instances concurrently, each declaring only its own plugin's tool | pass |
| 10 invocations, each answered by the instance addressed | pass |
| A plugin that exits at startup leaves 0 commands, module survives | pass |
| A plugin emitting non-JSON leaves 0 commands, module survives | pass |
| A collider on an already-allocated event base does not displace the owner | pass |

`test_pluggy_host.py` — the pluggy shim against **real pluggy 1.5.0**, 15 assertions, all
passing: MCP handshake, hookspec→tool reflection, `firstresult` returning a single value
while a plain hook returns the list, wrapper-only and unimplemented hooks correctly NOT
exposed, unknown hook/argument reported rather than silently empty, and all three pin
refusals (version mismatch, missing distribution, no plugin selected).

The claim being checked there is *"pluggy is not a protocol"*. It holds: the pluggy plugin
is started by the same Go module, over the same two stages, and its hooks arrive through
the same `DCMD`/`DCMR` declaration. There is **no pluggy-specific code above
`scripts/aimee-pluggy-host.py`** — the Go module needed no change at all to host it.

pluggy is vendored into the shipped package rather than installed on the target: `.252`
hosts a live aimee deployment and this must not alter its system packages.

## Bugs this found

**1. A dead plugin kept its commands registered.** The module checked
`client.Ready()` and served `client.Tools()` from cache. Neither changes when the plugin
process exits, so the module went on declaring commands for a plugin that was gone. The
first e2e run failed exactly there. Fixed by re-listing tools on every declaration rather
than trusting the cache — which also covers a hung or broken plugin, not just an exited
one — and detaching when the plugin cannot answer. This is the finding that justified
building the process-level test at all: every unit test passed with the bug present.

**2. Event kinds could not be package constants.** Found by reading
`bus_host_serve_kind()` (`core/event_bus/bus_route.c:109`) before testing: one event kind
binds to exactly ONE serving slot. Ten instances sharing package-level kinds would mean
the first attaches and the rest are refused. Corrected to a per-instance allocated pair.
The scale suite now asserts the constraint directly with a deliberate collider — and the
empirical result was **stronger than predicted**: the collider is denied at *attach*
(`mcp-collider attach: bus: attach denied`), not merely refused a serve binding.

**3. A use-after-free in the C driver**, found by review before it ran: the owned-command
backing was an array of structs, and the registry keeps `ud` pointers into it, so the
`realloc` on growth would dangle every previously registered command. Changed to
individually allocated entries so each `ud` is stable.

**4. The harness orphaned processes on failure.** Cleanup ran only at the end of a
successful `main`; a failing assertion calls `abort()` and never reaches it. Every failed
run therefore left its module processes (and their plugin children) running — found by
`pgrep` after the debugging runs, which had accumulated ~20 of them. This matters here
more than usual: `.252` hosts a live aimee deployment, and a test that leaks on failure is
exactly the one that will be run repeatedly while something is broken. Fixed with
`prctl(PR_SET_PDEATHSIG, SIGKILL)` in each forked child before `exec`, plus a
`getppid() == 1` check for the race window. Verified by starting a run, sending it
SIGABRT mid-flight, and confirming zero surviving `aimee-module-mcp-*` processes.

**5. A wrong assertion in the scale test itself.** It expected
`"served_by":"t0"` while Python's `json.dumps` emits `{"served_by": "t0"}` with a space.
The code was correct; the test was not. Recorded because a test that fails for its own
reasons is a test that will be silenced next time.

**6. A misleading leftover-process check in the runner.** `pgrep -f "$REMOTE_DIR"` matched
the shell running the `pgrep` itself, so a clean run reported a leftover PID. Narrowed to
match the module executables. A check that cries wolf gets ignored, which defeats it.

**Slice 4 note.** The refusal assertion is the one that matters, and it is checked by
observing that the code did not run rather than by trusting a status code: the refused
instance's plugin would create a sentinel file as its first action, and the file never
appears. Mutation-verified — flipping the verdict from refuse to allow makes the sentinel
appear and the assertion fire.

Two of Slice 4's three claimed deliverables turned out to be vacuous on inspection:
`plugin_permission_t` is never checked anywhere, so there was no permission enforcement to
be at parity WITH (a ceiling was added anyway, as a new control), and `kb_egress_admission`
is KB HTTP egress that `aimee.yaml` MCP clients do not use either. Only OSV was genuine
parity, and it is now the same function on both paths rather than a copy.

## Operator surface

`GET /v1/dashboard/metrics` carries a `plugins` array: one row per attached instance
with its event base, group, command count, state and last error. The state is the part
that matters — `refused` (blocked by the supply-chain gate), `silent` (admitted, plugin
gone), `pending`, `active`, `error` — because all of them otherwise present as "zero
commands" and each needs a different action.

## Reproducing

```sh
# local
make -C src build/obj/tests/unit-test-bus-plugin-process
make -C src build/obj/tests/unit-test-bus-plugin-scale
(cd server-go && go build -o ../src/build/obj/aimee-module ./cmd/aimee-module)
make -C src plugin-e2e AIMEE_MODULE_BIN=$PWD/src/build/obj/aimee-module \
  AIMEE_PLUGGY_PYTHONPATH=$PWD/src/build/pluggylib

# on .252: ships the two test binaries plus aimee-module, runs both, cleans up
sh scripts/run-plugin-e2e-on-252.sh
```

`plugin-e2e` is opt-in, not part of `make unit-tests`: it starts child processes and
needs `python3`.

## Slice 3: what "registered" was not doing

Finishing the registration slice turned up that a registered command was reaching far
less than it read like, and one of the gaps was introduced by an earlier draft of this
work:

- `aimee_command_find_method` and a command's own handler had **no production caller**,
  so a registered command was listed everywhere and invocable nowhere. `POST
  /v1/commands/<group>.<verb>` now dispatches through the registry.
- The manifest advertised a bare `/v1/commands/` prefix — **a route no client could
  reach**. That is precisely the "listed but unroutable" failure the registry exists to
  remove, and I had added it. Fixed to the full path and documented in the OpenAPI spec;
  `server-api-conformance-check` caught the undocumented route and now covers it.
- Registry commands reached the CLI manifest and **not MCP `tools/list`**, so a plugin
  was invisible to the clients that are the reason to host one. `mcp_group_tool_build()`
  existed for exactly this and had no callers.
- `tools.listChanged` has always been advertised and **never sent**, so a client that
  trusted the capability never re-listed. The registry now carries a generation and the
  stdio bridge emits `notifications/tools/list_changed` when it moves.

## Transport parity

`server-go/modules/mcp/sse.go` closes the last capability gap with the C client:
MCP over HTTP+SSE, covered by six tests (full session, bearer on every request,
relative-endpoint resolution, refusal of a stream that never announces an endpoint,
refusal of a non-2xx stream, and a module-level test proving the `Transport` seam
makes the two transports interchangeable). Before it, a plugin module could only
spawn a local process, so the C path could not be retired without deleting remote
MCP support outright.

## What this does NOT cover

- **Provisioning is covered, deployment is not.** `unit-test-plugin-grant-provisioning`
  runs the real provisioner and hands its output to the daemon's own grant parser
  (`bus_runtime_policy_load_dir`), so the file format the two sides must agree on is
  checked rather than assumed. Nothing yet starts a provisioned instance under a real
  `aimee-server`.
- **No live `aimee-server`.** The suites stand up a real bus host and runtime directly,
  which is the seam the modules actually talk to, but the daemon's own boot path
  (`obs_bus_configure_daemon_module_runtime` + grant files under `modules.d/`) was not
  exercised. The `.grant` files a real deployment needs are hand-equivalent to the
  in-test grant structs, not generated by anything yet.
- **SSE is unit-tested, not exercised on `.252`.** The process e2e suites drive
  stdio plugins; the SSE path is covered against an in-process HTTP server, not a
  real remote MCP endpoint.
- **No automatic re-attach.** A plugin that dies stays detached until its module is
  restarted.
- **The push path is not covered by an automated test.** The registry generation and the
  dispatch route are unit-tested, and the bridge's watcher is straightforward, but no
  test drives a live MCP client through an attach and asserts the notification arrives.
  That needs a daemon-backed harness the plugin suites do not stand up.
