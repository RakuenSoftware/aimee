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

## Re-run 2026-08-24: the decoders changed, so the probe had to

The end-to-end result below was made at an earlier commit. Production code has
changed since -- a new wire status, both scalar decoders now refusing a corrupt
cell instead of defaulting, the bus caller lifecycle -- so the run was repeated,
and the probe grew two checks for the part that changed.

A re-run exercising only what the previous one did proves the build compiles.
These two are the ones that could not have passed before:

```
  PASS  a corrupt boolean cell is refused, not read as false     status=bad_request
  PASS  a corrupt timestamp cell is refused, not read as absent  status=bad_request
```

Both used to answer the ZERO VALUE. `Atob` returned plain false for anything
unrecognised, so a malformed flag and a deliberate "no" were one value;
`textToTime` mapped an unparseable timestamp onto the zero time, which is exactly
what the encoder writes for a message that has none. In process the unit tests
assert the same thing -- what only hardware shows is that a frame carrying a
corrupt cell survives the transport intact and is refused by the MODULE rather
than mangled on the way.

**25 checks, green, twice consecutively, `probe exit=0`**, with delivery between
two sessions db1 holds unchanged. The full stack: `aimee-server` hosting the bus,
the config module, the C db1 module over SQLite, and this module wired to db1's
session family at `principal 1/67`.

One finding about the rig, which is this session's own ambient-state defect
turned on itself: the run script assumed grants already seeded in
`/etc/aimee/modules.d/server/`, because on the previous container an earlier
script had written them. On a fresh container there were none, the server never
started, and the failure looked like a missing bus socket. The script is
self-contained now.

Cleanup: CT 9097 destroyed and verified absent, watchdog stopped, source, scripts
and logs removed.

## END TO END: two sessions exchange a message

**The feature works.** On a clean Debian 13 container, against the current tree,
with the whole stack running -- `aimee-server` hosting the bus, the config
module, the C `db1` module over SQLite, and this module wired to db1's session
family -- **23 of 23 checks pass, twice consecutively, `probe exit=0`**:

```
2026/08/23 21:15 aimee module: session directory = db1 sessions (kind 11782) as principal 1/67

delivery end to end, between two sessions db1 actually holds:
  PASS  db1 holds session probe-a-9768                 db1 status=0 err=<nil>
  PASS  db1 holds session probe-b-9768                 db1 status=0 err=<nil>
  PASS  send between two directory-known sessions is accepted status=ok err=<nil>
  PASS  the send reply carries a stamped envelope      rows=1 err=<nil>
  PASS  the recipient's inbox reports exactly one message status=ok cells=[1 0]
  PASS  DELIVERED: the recipient drains the sender's exact text status=ok remaining=0 rows=1
  PASS  the drain reports nothing left behind          remaining=0
  PASS  the drained message is gone, not re-delivered  status=ok cells=[0 0]
```

Two sessions created in db1's own store over the bus, a message sent by one,
carried across the real transport, landing in the other's inbox with the exact
text, and gone after draining. Nothing registered those sessions with this
module: it learned they exist entirely from db1.

### What the end-to-end run found

- **Sessions the directory vouches for could not send.** `Send` required a LOCAL
  entry, so a session db1 knew about was refused until some message had already
  touched it -- a conversation that could never start. The local map holds
  inboxes and labels, which is this module's state; existence is db1's. Admission
  through the directory now creates the entry on first use.
- **The channel stage did not follow.** Fixed `Send` and not `ChannelSend`, so
  one sender got two different verdicts depending on which stage it used: it
  could message a peer directly and not a channel. Caught by the probe on
  hardware, and now `Reply`, `Ask`, `ChannelJoin` and `ChannelSend` all admit the
  same way.
- **The probe reported a module that was not running as one WITH a directory.**
  A `capability absent` transport error leaves the status word at its zero value,
  which is `StatusOK`, so `ok != no_directory` read as wired. It now refuses to
  judge anything until the call was actually served.
- **A segfault after "all checks passed".** The probe's caller polls the bus's
  shared-memory region from its own goroutine, and `Detach` unmaps it.
  `CloseAndWait` says in as many words that it "must run before the underlying
  Client is detached"; the probe never called it. Every check green, then a fault
  in `Control.Epoch` reading through an unmapped page. Nothing in process can
  find this -- there is no region to unmap, so the rule has nothing to enforce it.

### Two characteristics, stated rather than left to be discovered

**An absent session reports `unavailable`, not `no_peer`,** because the C store
returns -1 for a bad argument, a dead connection and a missing row alike. The
probe accepts either and names which it saw, since failing on a defect in another
component teaches people to skim past failures. The Go store reports `missing`,
and this becomes `no_peer` when db1 runs it.

**A directory lookup blocks the handler.** With `db1` killed underneath a live
module, a send waits on the directory call and the caller's own deadline expires
first, so it sees a timeout rather than the `unavailable` the module would
eventually return. This is the hazard `module.go` warns about in its own header:
a handler that waits holds one of sixteen in-flight slots. It matters only when
the store is unreachable, and it is a real cost of reading existence remotely.
Recorded rather than patched at the end of a session, because a cache or a
shorter deadline is a design decision and both need their own tests.

## Re-run against the current code

The run recorded below was made at an earlier commit. Everything the module
ANSWERS has changed since -- session-scoped stages now report `no_directory`
rather than `unknown_sender` or `no_peer` -- so the record was re-made on a fresh
container against the current tree. Same shape, different answers, and three new
findings about the rig itself.

**Result: sixteen checks, green, twice consecutively, `probe exit=0`.** The lines
that matter, quoted from the run:

```
  PASS  delivery stage (kind 12033) reachable          err=<nil> status=no_directory
  ....  module has NO session directory; peer messaging is inert here
  PASS  module states whether it has a session directory wired=false
  PASS  unknown sender refused as a SERVED call        status=no_directory
  PASS  members of an absent channel answers OK with none status=no_directory cells=[]
  PASS  grant is readable back (module holds state)    status=ok cells=[1]
  PASS  malformed frame refused at the transport level err=module call failed with status 4
```

The channel row is the one worth reading twice. It previously answered `ok` with
no members -- a successful, healthy-looking reply from a module where no channel
can ever have a member, which is what made the inert module invisible. It now
answers `no_directory` over a real bus, and the startup line

```
aimee module: no session directory configured; peer stages will answer
no_directory until one is wired (grants still served)
```

appeared in the module's log, which is the first time that code has executed
anywhere but a unit test.

### What the re-run found about the rig

- **A module that SERVES stages needs `serve=`, not `request=`.** The grants
  written for the earlier run used `request=` for the module itself, which is
  what a CLIENT of those kinds declares. Found by reading the config module's own
  `grants/module.grant.in`, which carries `serve=4609`.
- **`aimee-server` will not start without the config module.** It exits 1 after
  `config_wait_ready(10000)` with "server startup rejected invalid
  configuration", and logs to `$AIMEE_HOME/server.log` rather than stderr, so at
  `--log-level=debug` on a bare console it is silent. `aimee-module-config` is a
  separate program from an external repository and has to be built and started
  first.
- **The supervisor did not spawn the module in this hand-built rig**, because no
  `server.modules` manifest was installed -- that manifest is what it spawns
  from, and this rig seeded grants only. So the module was started directly, and
  spawn ELIGIBILITY was checked separately and statically, by the same
  computation `export_c_repositories.py` performs: `aimee` is in the canonical
  inventory's `required` list, so `enabled` is true and it reaches
  `server.modules`. That is the fix for the defect the earlier run found, and it
  holds.

One check is weaker than its name suggests: "unadvertised stage 9 is not served"
passed with `module call deadline exceeded` rather than an explicit refusal. Not
being served manifests as a timeout here, not as a clean no, and the check
accepts any non-nil error.

**Not exercised, again and for the same reason.** Peer-to-peer delivery between
two live sessions. The module has no directory and nothing registers a session,
so there is no configuration of this rig in which a message could be delivered.
Every row above is consistent with that, which is the point of the row that now
reports it explicitly.

Cleanup: CT 9095 destroyed and verified absent, watchdog stopped, source tarball
and logs removed, no processes left. Other sessions' containers untouched.

---

Sixteen checks, green on consecutive runs. The count and the rows are asserted
against the probe by `TestValidationRecordMatchesTheProbe`, because this table
was hand-transcribed from probe output and dropped a row: the run made fifteen
checks while the record claimed fourteen. An evidence table nothing checks
drifts from the run it describes, and drifts quietly, since a doc has no build
to fail.

| Check | Result |
|---|---|
| delivery stage 12033 reachable | pass |
| module states whether it has a session directory | pass |
| unknown sender refused as a SERVED call | pass |
| inbox stage 12034 reachable | pass |
| unknown session refused, not reported as an empty inbox | pass |
| grant stage 12035 reachable | pass |
| absent grant answers OK with a false value | pass |
| grant write accepted | pass |
| grant is readable back (module holds state) | pass |
| grant did not leak in the reverse direction | pass |
| take on an unknown session is refused | pass |
| malformed frame refused at the TRANSPORT level | pass |
| channel stage 12036 reachable | pass |
| channel send by an unknown sender refused | pass |
| members of an absent channel answers OK with none | pass |
| unadvertised stage 9 is not served | pass |
| a corrupt boolean cell is refused, not read as false | pass |
| a corrupt timestamp cell is refused, not read as absent | pass |
| db1 holds session (probe-a and probe-b) | pass |
| send between two directory-known sessions is accepted | pass |
| the send reply carries a stamped envelope | pass |
| the recipient's inbox reports exactly one message | pass |
| DELIVERED: the recipient drains the sender's exact text | pass |
| the drain reports nothing left behind | pass |
| the drained message is gone, not re-delivered | pass |

The load-bearing result is the pair in the middle. A domain refusal
(`unknown_sender`, `no_peer`) arrives as a **successful** bus call carrying a
status in the body, while a malformed frame arrives as a transport error
(`status 4`). That three-way distinction is what lets a governance tap tell "the
module is broken" from "the module said no", and it could not be tested in
process because in process there is no transport to distinguish it from.

`aimee-module-aimee` is a supervised long-lived process holding its own state:
the grant written by one probe invocation is still there for the next.

## Both services up (2026-08-24, current tree)

Three re-runs of this validation stood up `aimee-server` and never `aimee-kb`,
while the server's own log said `knowledge service unreachable` each time and I
read past it. Both are up here, on one container, and the numbers are what the
run printed rather than what it was expected to print.

```
server=1 kb=1 db1=1 aimee=1 config=2
/run/aimee/bus.sock      the server's module bus
/run/aimee/kb-bus.sock   the KB's own module bus

aimee-kb      LISTEN 127.0.0.1:8790,  GET /v1/health -> 200
aimee-server  /v1/health -> 200 over /var/lib/aimee/aimee-http.sock
PostgreSQL 17 + pgvector 0.8.0, 253 tables in public
```

**The KB reports itself DEGRADED, and that is the honest headline.** Its health
body:

```
"status":"degraded"
"db2_ok":false, "db2_kb_tables_ok":false, "pgvec_ok":true
"warnings":["DB2 schema not ready"]
"blockers":["store unavailable: the KB database schema is not ready ...",
            "no embedder configured ..."]
```

The embedder blocker is expected and already a stated non-goal here. The schema
one is not: `--bootstrap-db2` had just reported `{"status":"ok",
"knowledge_ready":true}` and created those 253 tables, and they are in `public`,
which is `current_schema()`, and they include `kb_async_jobs`, `kb_doc_assets`
and the rest of the `kb_*` set. So the KB's bootstrap and the KB's health check
disagree about the same database on the same connection string.

Not diagnosed further. One hypothesis worth someone's time rather than mine at
this point: the bootstrap reported `"config_saved":false`, so the URL was never
persisted, and a health check reading it from config rather than the environment
would be checking nothing. That is a hypothesis, not a finding -- it is written
down as one so nobody later reads it as the second.

**The server never reached the KB**, and said so with the KB up and answering:
`knowledge service unreachable (transport /v1/health)`. This rig never told the
server where the KB is. The earlier run below had them in two containers with an
address between them; one container with two loopback listeners is not the same
arrangement, and the KB client resolves an endpoint that was not configured here.

**Peer messaging is unaffected and green throughout**: 25 checks, twice
consecutively, `probe exit=0`, with both services running. The feature does not
touch the KB, which is why it is unaffected -- stated because "green while the KB
was degraded" would otherwise read as coverage it is not.

### The earlier run, at commit `ffb60ac`

Kept because it is the only run where the two services were in SEPARATE
containers, which is the arrangement the KB client expects.

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

- **Peer messaging end to end between two sessions.** Stated too gently when
  this was written. It is not that the run did not happen to exercise delivery;
  the module as built COULD NOT DELIVER AT ALL, and this run could not tell.

  There is no `DirectorySource`, and nothing else populates the registry either:
  `Register` has no caller outside tests and no bus op reaches it. So no session
  could exist, and every session-scoped check above was made against a module
  where none ever would.

  Those checks still passed, and had to. A module that correctly refuses an
  unregistered sender and a module that can never have a sender produce the same
  word -- `unknown_sender`. The channel row is the sharpest: "members of an
  absent channel answers OK with none" is a SUCCESSFUL, healthy-looking reply,
  and it read as the feature working.

  The module now answers `no_directory` for session-scoped stages, which is a
  fact about the module rather than about the caller's session, and the probe
  establishes which configuration it is talking to before asserting any refusal.
  The rows above were produced by the UNWIRED configuration; a wired one asserts
  different statuses for the same checks.
- **Retrieval quality on the kb.** The embedder is a stub returning hash-derived
  vectors with no semantic content. It proves the service starts and the wire
  works; any relevance measured against it is noise.
- **The server reaching the KB.** Both services ran and answered `/v1/health`
  independently, and the server still reported `knowledge service unreachable`
  because nothing configured a KB endpoint for it. So "both services up" is
  true and "the two are integrated" is not, and only the first was tested.
  RESOLVED on CT 9099 -- see "The two loose ends closed" below. It was a rig
  gap: `AIMEE_KB_API_URL` was never set, and the server had not been told where
  the KB was.
- **The KB's store.** It reports `status: degraded` with `DB2 schema not ready`
  while 253 tables sit in the schema it uses. Its bootstrap and its health check
  disagree, and which of them is right was not established.
  RESOLVED on CT 9099 -- see below. Also a rig gap, and the premise here was
  wrong: they do not disagree. The health check is a bus call to the postgres
  module, which was not running; bootstrap goes straight through libpq.
- **The in-image PostgreSQL.** The kb was pointed at the container's stock
  cluster via `AIMEE_DB2_URL` because the internal one refuses to run as root,
  which is PostgreSQL's rule rather than aimee's.
- **`workflows` and `wfe`**, whose grants were skipped, and the LLM synthesis
  lane, which has no endpoint configured.
- **The ref 30 absorption.** This run validated the module at principal ref 31,
  stages 1/2/3/4, kinds 12033/12034/12035/12036. The agreed end state folds
  peer messaging into ref 30 as stages 20/21/22/23 (kinds
  11796/11797/11798/11799). All FOUR stages renumber, and an earlier draft of
  this note listed only three. Following it would have left the channel stage
  at 4 under ref 30, which is kind 11780, and 11780 is not a free number: it is
  `db1-agent-work`. The off-formula check in `New` would NOT have caught that,
  because 11780 is exactly what the formula gives for ref 30 stage 4. What
  catches it is the duplicate-stage check, since after the absorption db1 and
  peer messaging are capabilities of ONE module, so the two stage 4s meet at
  construction and `ErrStageConflict` refuses to build the module at all.

  That is the intended outcome and worth stating, because it is the reason the
  conflict is a startup failure rather than agent-work traffic being served by
  the channel capability. It also depends on db1 arriving as a capability of
  this module rather than beside it: two modules at one ref never compare stage
  tables, and nothing would refuse. The
  mechanism is proved; those numbers are not final and the run must be repeated
  after the renumber.

## The two loose ends closed (2026-08-24, CT 9099)

The section above listed two things the run left unexplained: the KB reporting
`DB2 schema not ready` over a schema holding 253 tables, and the server
reporting `knowledge service unreachable` while the KB answered on 8790.

Both were read out of the source rather than guessed at, and **neither was a
code defect. Both were gaps in the rig** — things that run never did.

**`db2_ok: false`.** `kb_module_postgres_health_probe`
(`src/kb/kb_module_stage_adapters.c:581`) is a `call_module` to
`AIMEE_POSTGRES_EVENT_HEALTH` — a *bus call to the postgres module*
(`runtime: go`, ref 28, kind 11265, `placements: ["kb"]`). No postgres module
was running on the KB's bus, so the probe had nobody to ask. The KB's *pool*
connects directly through libpq, which is exactly why bootstrap succeeded and
253 tables exist. Bootstrap and health check were not disagreeing; they take
different paths, and only one of them had been stood up.

**`knowledge service unreachable`.** `kb_client_v1_base_url`
(`src/modules/kb_client/kb_client.c:1502`) reads `AIMEE_KB_API_URL` and returns
`NULL` when it is unset or empty. The rig never set it. The server was not
failing to reach the KB; it had never been told where the KB was.

CT 9099 was built with both closed — the postgres module started on the KB's
own bus (daemon `"kb"`, its own grant directory and its own config module), and
`AIMEE_KB_API_URL=http://127.0.0.1:8790` in the server's environment.

| | CT 9098 | CT 9099 |
|---|---|---|
| `db2_ok` | false | **true** |
| `db2_kb_tables_ok` | false | **true** |
| `pgvec_ok` | true | true |
| `warnings` | `["DB2 schema not ready"]` | **`[]`** |
| `knowledge service unreachable` lines | ≥1 | **0** |

`FLEET: server=1 kb=1 postgres=1 db1=1 aimee=1 config=2`, and peerprobe passed
all checks with exit 0 on two consecutive runs against that fleet.

### The zero was confirmed positively, not by absence

A count of zero unreachable lines is the weakest possible evidence: a server
that never probes at all produces the same zero. So the same container ran both
arms, one process at a time, identical in every respect but the URL:

| `AIMEE_KB_API_URL` | `knowledge service unreachable` lines |
|---|---|
| `http://127.0.0.1:9999` (nothing listening) | **1** |
| `http://127.0.0.1:8790` (the real KB) | **0** |

The dead-URL arm establishes that the check *can* fail and *does* speak up, so
the zero in the other arm is a result rather than a silence. After the restore,
the peer modules were brought back and peerprobe was run once more against the
restored server: all checks passed, including `DELIVERED: the recipient drains
the sender's exact text`.

The first attempt at this control **failed and still printed something that
looked like an answer** — it backgrounded the restarts inside a single
`pct exec`, which killed them when the call returned. The server was down, so
"no unreachable lines" was true for entirely the wrong reason. That is the
session's own recurring defect shape turned on its own instrument: a
measurement that broke and whose output was still shaped like a verdict. It was
caught by checking that the server was actually running before reading the
count, which is why `server up:` appears beside every count in the table above.

### What is still not covered

`embed_ok` remains false and the KB still reports `status: degraded`, on one
blocker: `no embedder configured: set embedder_model (or EMBEDDER_URL)`. That
is the documented non-goal for this work, not a finding.

### Teardown

CT 9099 destroyed and purged, its watchdog stopped, and every rig file removed
from `/root`. Verified afterwards: CTs 9095–9098 and 9099 all gone, no rig
files remaining. The containers left on the host (9001, 9078–9080) belong to
other sessions and were not touched.

## The surface, and the defect it exposed (2026-08-24, CT 9100)

Every run before this one tested the ENGINE. A probe binary written for the
purpose spoke to the module over the bus and messages moved, which proved the
module and not the feature: nothing in the product called it, so no session
could. `peer_send` and `peer_inbox` in the MCP tool table are that missing
surface, and CT 9100 is the first run where the product itself is the caller.

The result is that the surface works and the feature does not, for a reason
nothing before could have found.

### What was proved

- **The grants ship from the contract.** `server-peer` (ref 68) and `aimee-db1`
  (ref 67) were generated into the deployed bundle by
  `export_c_repositories.py` from `process-contracts.json` and installed
  unmodified. Binaries were placed at the paths those grants name, so the test
  exercised the shipped wiring rather than a hand-edited fixture.
- **Real sessions register themselves.** `mcp_session_register` runs before tool
  dispatch, so a session becomes addressable by making its first MCP call. db1
  held `('peer-alpha','mcp','uid:0')` and `('peer-beta','mcp','uid:0')` after
  two `/v1/mcp/call` requests carrying those session ids. No fixture wrote them.
- **The client's frames are correct.** With the module instrumented to log every
  arrival, every server-originated call landed as `stage=1 op=1 cells=6` (send)
  or `stage=2 op=3 cells=2` (take) — exactly the declared arities.
- **The refusals arrive, and differ.** `self`, `too_long`, and `no_directory`
  all came back as named domain refusals through the MCP surface.
- **The three-outcome discipline earned itself.** Every failure below reported
  as "the peer-messaging module did not answer ... This is not a refusal", which
  is what made it instantly separable from a domain no. Had the client collapsed
  the two, this would have read as the module rejecting the messages.

### The defect

**A `peer_send` originated by aimee-server enters the module's handler and never
returns.** With the module logging every invocation, a server-originated send
logged its arrival and then nothing at all: no success, no refusal, no encode
error. The handler is wedged inside `Registry.Send`, whose first act is a nested
bus call to db1 to resolve the sender.

What it is NOT, each ruled out by measurement rather than by argument:

| ruled out | how |
|---|---|
| a timeout | the client deadline was raised 5s → 30s and rebuilt; five sends still failed, and returned in about a second between them |
| a malformed frame | the module logged `cells=6`, the declared arity, on every call |
| module unavailability | the module logged the arrivals, so the bus routed them |
| grant admission | `self` and `too_long` refusals crossed the same grant, same stage |
| my client's decoding | the failures are on paths where the module never replied at all |

And the control that makes it specific: **`aimee-peerprobe`, an external bus
client, ran the same operation against the same module process at the same
instant and delivered successfully every time** — including `DELIVERED: the
recipient drains the sender's exact text`. Same module, same db1, same second.
The variable is which process originated the call.

One further observation, from the ordering in the logs: `peer_inbox`, which
makes ONE nested directory lookup, succeeded repeatedly — until the first
`peer_send` hung, after which later lookups either hung too or returned
`unavailable` fast. So the first wedge appears to poison the module's directory
path for subsequent calls rather than failing in isolation.

**The mechanism is not established.** Why a nested module call behaves
differently when the daemon is the originator is a question about the bus host
(`src/modules/audit/obs_bus.c` and the core event bus), not about peer
messaging, and it is not answered here. Worth noting that the pool comment in
`obs_bus.c` already describes this class of fault in a different guise — "the
review waits on its own callback and nothing moves until something times out" —
which was addressed by giving each concurrent call its own client. This looks
like the next instance of the same shape, one level deeper, and the earlier fix
does not cover it.

### What this costs

Peer messaging between two real sessions does not work through the product
today. The module is correct, the client is correct, the wiring is correct, and
the path from the daemon into a module that must itself call another module is
not. Nothing in this repository could have found that before the surface
existed, which is the argument for having built it.

### Rig faults, recorded because they nearly became findings

- The first build **reported COMPLETE while `aimee-server` and `aimee-kb` did
  not exist** — the make steps piped through `tail` and carried no `|| exit 1`.
  The rebuild checks every step by exit code and stops on the first failure.
- The grant check used `realpath`, which SUCCEEDS on a path that does not exist.
  It printed "ALL GRANT EXECUTABLES RESOLVE" while five were missing, and the
  bus then refused the entire policy directory. `[ -x ]` is the test.
- `pgrep -fc "aimee-module-db1"` matched **its own command line**, inflating
  every fleet count by one and briefly suggesting duplicate modules. The
  `[/]`-bracket form was on the server and kb patterns and not the module ones.
- `pkill -f aimee-module-aimee` matched its own `pct exec` shell and killed it
  (exit 143) — the third instance of the same self-match this session.
- Two frames refused as `ModuleStatusInvalidRequest` were **the probe's own
  deliberate malformed-frame check**, not a client fault. Nearly attributed to
  `peer_client` before the timestamps were lined up against the probe's run.

### Teardown

CT 9100 destroyed and purged, watchdog stopped, every rig file removed from
`/root`, verified afterwards. The containers left on the host (9001, 9078–9080)
belong to other sessions and were not touched. The 30s-deadline edit and the
module's DIAG logging existed only inside the container and are not in the tree.

## Root cause, and the feature working (2026-08-24, CT 9101)

The previous section reported a `peer_send` that "enters the module handler and
never returns", and named the bus host as the likely owner. **That reading was
wrong, and it was wrong because of a defect in the diagnostic rather than in the
reasoning.**

Two of the measurements behind it had broken:

- The timing harness printed `0.00s` for every call because `bc` was not
  installed in that image. "Fast" was read off a timer that was not running.
- The conclusion "only one obs_bus failure was logged" came from a log captured
  BEFORE the runs it was applied to. `obs_bus_log_module_call_failure` is not
  rate-limited; the lines were simply not there yet.

With both fixed, the answer took one run.

### The client could not say which failure it had

`peer_client` collapsed five conditions into one sentence — "the module did not
answer" — and logged them once per PROCESS. The first failure claimed the single
warning and every later one was silent, including a DIFFERENT failure needing a
different repair. That is why an absent module and a rejected reply were
indistinguishable for six container runs.

Fixed: the `aimee_module_call_result_t` is carried out through a `transport`
out-param, named (`capability_absent`, `deadline_exceeded`, `capability_denied`,
`protocol`, …), reported in the text a model reads, and warned once per DISTINCT
code. A reply that arrived but could not be decoded reports `protocol` rather
than an absent module — the module demonstrably replied, so sending the reader
to check whether it is running is the wrong instruction.

The very next run said `protocol`, **in 24 milliseconds**. Not a hang. The
module replies and the client rejects the reply.

### The defect

`peerwire.Btoa` writes `"1"`/`"0"`. The C row reader accepted only `"true"` and
`"false"` — words `Btoa` NEVER writes — so it rejected **every message row the
module has ever sent**.

`peerwire.Atob` is lenient and accepts `"true"`/`"false"` as well, so the C
client's REQUESTS were understood and only the reply direction broke. Half a
conversation working is worse than none: at the caller it reads as the far side
failing rather than as a grammar this side got wrong.

Both test suites were green throughout. The C fixture spelled the cell
`"false"` — it had been written from the same misreading as the code it was
meant to check, so every assertion passed against a row that cannot come off the
wire. And `cwire_test.go`, the cross-language pin, checked the status numbers and
the row width and stopped there: the two sides agreed on how many cells a row
has and disagreed about what is IN one.

### The guards that now exist

- `TestCClientSpeaksTheSameBooleanGrammar` asserts, against `Btoa`'s real output
  rather than a copied list, that whatever the module can write the C side can
  read — and that whatever the C side writes, `Atob` accepts. Demonstrated red by
  changing the C literal.
- Five C checks drive every spelling `Atob` accepts through the reader.
- The C fixture now carries the row the module actually sends.
- Mutation: reverting the reader to `"true"`/`"false"` turns `a zero status with
  a full row is a delivery` red.

### The feature, working

Through `POST /v1/mcp/call` — the request `aimee mcp-serve` posts for a real MCP
client — with sessions the server registered itself:

```
gamma -> delta:   Delivered to delta (message pmsg-3, conversation conv-4).
delta drains:     1 msg; 'gamma speaking' from gamma
delta -> gamma:   Delivered to gamma (message pmsg-5, conversation conv-6).
gamma drains:     1 msg; 'delta answering' from delta
delivered ONCE:   0 message(s) taken; 0 still waiting.
```

Three sessions, not two, so the pair is not a special case: `epsilon drains: 2
msg from ['delta', 'gamma']`. An 8192-byte body arrives with all 8192 bytes
intact and 8193 is refused `too_long`. `self` refuses. peerprobe passes twice
with **exit 0 captured before any pipe** — the earlier `probe exit=$?` after
`| tail` had been reading tail's status.

Negative control: with the module stopped the same send answers
`deadline_exceeded` and with it back it delivers, so the success is not an
absence of checking. `FLEET server=1 kb=1 pg=1 db1=1 aimee=1 config=2`,
`db2_ok True`, `warnings []`, zero knowledge-unreachable lines, and exactly one
`peer.client` line in the whole log — the negative control's own.

### Known and NOT fixed here

**A send to a session that does not exist answers `unavailable`, not
`no_peer`.** That is correct given the store: `db1_server_session_get` returns
`-1` for both "no row" and "store broken", and db1's own stage comment says
mapping that to MISSING "would report a broken store as nothing recorded". The
conservative answer is the right one, and its cost is that a caller is told to
retry for a peer that will never exist. The repair belongs to db1's read
contract — changing that return would touch every caller — and db1 is being
absorbed into Go by another session, so it is raised there rather than patched
around here.

**peerprobe moved from principal ref 69 to 200.** 69 collided with the
control-plane module's outbound identity, allocated by another session, and the
collision surfaced as `attach denied` in the probe — a validation run reporting
a bus problem that was really bookkeeping two repositories away. The guard that
was supposed to prevent this read the contract for refs already DECLARED, which
cannot see a ref someone is about to take; it now enforces a floor far above the
range the contract allocates from, and fails if the contract ever grows into it.

### Teardown

CT 9101 destroyed and purged, watchdog stopped, every rig file removed from
`/root`, verified. Containers 9001 and 9078–9080 belong to other sessions and
were not touched.
