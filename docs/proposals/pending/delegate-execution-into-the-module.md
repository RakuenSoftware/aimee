# Proposal: Move delegate execution into the delegates module

- **State:** PENDING — decided architecture, port not started.
- **Date:** 2026-08-10.
- **Charter roles:** Enforce / Constrain-Verify.
- **Archived parent:**
  [`delegate-budget-must-fit-its-stage-cap.md`](../done/delegate-budget-must-fit-its-stage-cap.md).
- **Supersedes:**
  [`delegate-limit-diagnostics-residual.md`](../rejected/delegate-limit-diagnostics-residual.md).
- **Thesis:** delegate execution is logic, so it belongs in the delegates module and is
  reached over the event bus. The C daemon must not serve it, and the agent-service HTTP
  socket it is served on today is neither bus traffic nor part of aimee-server's external
  `/v1` surface, so it goes away.

## 1. What is actually wrong

`server-go/modules/roundtable/reviewer.go` imports `server-go/modules/delegates/plane` and
calls it over HTTP. `plane/client.go` holds an `http.Client` and a `net.Dialer` doing
`DialContext(ctx, "unix", socket)`. It is wired live at `server-go/cmd/aimee-module/main.go:60`.

The socket is `AIMEE_AGENT_SERVICE_SOCKET` (see `src/server/roundtable_review_bus.c:109`),
which is the **C daemon's** agent service. So this is not one Go module calling another: it
is a Go module reaching back into the C daemon over HTTP.

That is precisely the anti-pattern `bus.ModuleCaller`'s own documentation names
(`server-go/bus/module_caller.go:18-21`):

> the sole module caller was the C daemon that hosts the bus, so any other Go process had
> to reach a stage through that daemon's HTTP API rather than over the bus it was already
> running on.

It breaks three rules at once: modules get no HTTP, modules never talk to each other
directly, and every inter-module exchange goes over the bus so governance can see it.

## 2. Why roundtable cannot simply be rewired

There is no bus stage to point it at. `server-go/modules/delegates/delegates.go` is 49
lines and canonicalizes role names — its own comment says it "canonicalizes a delegate role
**without invoking a delegate**". Execution is ~12,811 lines of C under
`src/modules/delegates/`.

So roundtable's HTTP client is a *symptom*. It can only be deleted once execution is
reachable as a stage, which means the port below is the prerequisite, not a follow-up.

Serving execution from the C daemon as a bus stage was considered and **rejected**: the
daemon owns the bus, mTLS/MCP, the external HTTP surface and the audit tap, and nothing
else. Delegate execution is none of those.

## 3. The contract

A delegate call is: **persona / model / prompt in, response out**, plus the delegate's own
`working / failed / done` status.

Nothing about the caller crosses. `ReplayOnly`, `ExecutionVersion`, `WorkItemID`, `Stage`,
`DurableSlot`, `RetryTag` and `Participant` — every one of which `plane.DelegateRequest`
carries today — belong to whoever owns work items. A retry is not a field a delegate
interprets; it is the caller choosing to call again. Fifty resumptions are fifty
independent calls that know nothing of each other.

`plane.DelegateRequest` is therefore the wrong starting point for the wire contract. It is
an HTTP API shaped around a workflow client, not a module contract.

A caller may still put state-derived *content* in the request — "we have X, Y and Z, we now
need A, B and C" is input the delegate consumes and forgets. What it must never do is make
the delegate a custodian of the caller's state.

## 4. Ordering

The port is large, so it needs slices that each land green. Two properties decide the
order: a slice must be cuttable over in one commit (never leave a rule in two languages),
and it must not require the execution entry point to move before its dependencies.

Measured sizes, smallest first, as a starting decomposition:

| Candidate | Lines | Shape |
| --- | --- | --- |
| `delegate_role.c` | 237 | already half-ported; the alias table is duplicated in `module_adapter.c` |
| `delegate_backend.c` | 250 | backend registry |
| `delegate_economics.c` | 343 | cost/tier report over job+task rows |
| `delegate_routing.c` | 483 | which delegate for a role |
| `delegate_launch.c` | 488 | `delegate_launch_coord_job` — the entry point |
| `delegate_prompt.c` | 1,982 | prompt construction |
| `delegate_backend_docker.c` | 1,635 | sandbox backend |

The pure decisions (`routing`, `economics`) port first and prove the stage shape. The
backends move last: they are where the real I/O lives, and they are what makes the entry
point meaningful.

`delegate_role.c` is a special case worth resolving early. Its alias table is byte-for-byte
duplicated in the module adapter — the existing comment in `test_process_module_handlers.c`
records that nothing in the build keeps them in step, and that the duplicate exists because
the thin client hosts no bus. Deduplicating it needs a decision about whether the thin
client should canonicalize roles at all.

## 5. Acceptance

- `server-go/modules/roundtable` contains no HTTP client and no import of another module.
- Delegate execution is reachable only as a bus stage; `AIMEE_AGENT_SERVICE_SOCKET` and
  `AIMEE_AGENT_SERVICE_URL` are gone.
- A delegate holds no caller state — only its own `working / failed / done`.
- `check_go_module_boundary.py` lands with an **empty** allowlist, forbidding
  `server-go/modules/<a>` from importing `server-go/modules/<b>`.

That last one is the point of the whole exercise: today the rule is a sentence, and the one
violation of it was live in the shipping module process. When the check exists with nothing
in it, the rule stops depending on anyone remembering.
