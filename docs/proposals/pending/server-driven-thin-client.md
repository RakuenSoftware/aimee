# Proposal: Server-driven thin client — the client knows transport, nothing else

- **State:** proposed.

## Thesis

The thin client is supposed to be a presentation surface. It is currently a second
implementation of the API.

It carries a 245-entry map of `method -> /v1 route`, a set of argument marshallers, a help
table, and a command table. None of that is knowledge the client can hold correctly, because
all of it describes **what the server can do** — and the server it is talking to is a
different build, deployed separately, on another host.

The client's only legitimate concerns are the ones that are genuinely local: reaching the
server (endpoint resolution, TLS/mTLS, bearer), handing over the invocation, rendering the
result, and exposing the local resources the server cannot reach on its own (the terminal,
and the workspace filesystem via the existing reverse-channel).

Everything else moves to the server. The test of success: **a new server capability is usable
from an existing client with no client change.** A client upgrade should be required only when
the transport or the wire contract itself changes — a large upgrade, not a routine one.

## §0 What already exists

Most of the machinery is present. This proposal is mostly deletion plus one endpoint.

- **`command_registry.h` states the right invariant, but it is not in force yet:**

  > "A capability is registered ONCE, here, by the module that owns it. CLI, the v1 RPC
  > routes, MCP and ACP all route from this table. … A surface enumerates the registry; it
  > does not keep a list of its own."

  **Measured 2026-08-17: nothing registers.** `aimee_command_register` and
  `aimee_command_register_many` are called only from `tests/test_command_registry.c`. The one
  production reader, `modules/protocols/mcp/mcp_group_tool.c`, therefore enumerates an empty
  table. The registry is a correct design with no data in it, and the four hand-maintained
  tables it was written to replace are still the live sources.

  This matters for phasing: **the registry cannot be the thing the server serves definitions
  from until something populates it.** Either populate it first, or serve each kind of
  definition from wherever it actually lives today (§1). Assuming the registry is authoritative
  is the mistake to avoid — its docstring reads as though it already is.

- **The server already publishes its own API**: `/v1/openapi.json` and `/v1/openapi.yaml`.
- **The workspace reverse-channel already exists** (`cli_workspace_reverse_channel_start`,
  `modules/workspace/cli_workspace_serve.c`). This is the usual objection to a dumb-pipe client
  — "the server cannot read the user's files" — and it is already solved and in use by
  `mcp-serve`.
- **The marshalling grammar is already declarative.** `marshal_request` dispatches through
  tables (`MARSHAL_NO_ARGS[]` and siblings) plus prefix fallbacks, not bespoke code per
  command. Argument shape is already data; it is simply data living in the wrong binary.

## §1 The coupling, measured

Four parallel bodies of server-derived knowledge in the client:

| What | Where | Size |
|---|---|---|
| `method -> /v1 route` map | `CLI_V1_GEN_ROUTES`, `cli_v1_routes_d.c` | 245 entries |
| argument marshallers | `marshal_request` tables, `cli_v1_routes_b.c` | table-driven + prefix rules |
| help text | `client_help[]`, `cli_help_data.h` | 67 entries |
| command table | `cmd_table.c` | 82 entries |

Three build-time guards keep them agreeing: `gen-cli-v1-routes.py` generates the route map,
`check-cli-v1-routes.py` diffs it against the registry, `check-cli-help-coverage.py` requires a
help entry for every routed command.

The guards work. That is not the problem. **The problem is what they can see.** From
`check-cli-v1-routes.py`:

> "so a registry change that isn't reflected in the committed map fails `make lint`"

They compare *the source tree against itself*. They cannot compare a **deployed client**
against a **deployed server**, which is the only comparison that matters at runtime. Each
guard's docstring also records a breakage that shipped anyway — commands that routed but had
no marshaller, commands that worked but were invisible to `help`. Four tables and three
checkers is the cost of holding this knowledge in two places; it is a permanent tax, not a
one-off.

### The failure this produces

Observed on an operator's machine (2026-08-17): client `pre-merge-safety-1696`, server
`pre-merge-safety-2020` — 324 commits apart. Every capability added server-side in that window
was unreachable from that client, and the client cannot say so usefully, because it does not
know what it is missing. It reports "has no /v1 route", which reads as "that command does not
exist" rather than "your client is old".

The generation step also means the *repo* is always self-consistent, which hides the skew
during development. It only appears in the field.

## §2 Target: one invocation endpoint

The client sends the invocation. The server parses, routes, validates, executes, and returns a
presentation. The client prints it.

```
POST /v1/cli
  { argv: [...], cwd, term: {tty, width}, stdin?: ..., json: bool }
->  a presentation stream + exit code
```

What that deletes from the client: the route map, the marshallers, the help table, the command
table, argument validation, response profiles, and the three drift guards — because there is no
longer a second copy to drift.

`help`, `--help`, completion, and "did you mean" all become server responses. `aimee help` on an
old client against a new server lists the **new** server's commands, correctly, with no client
change. That is the acceptance test for the whole proposal.

MCP follows the same rule: `tools/list` and `tools/call` proxy to the registry rather than
consulting any local tool table.

## §3 What legitimately stays client-side

This is the boundary, and it is short:

- **Transport.** Endpoint resolution, TLS, mTLS client identity, bearer tokens, retry/timeout.
  This is the client's actual job.
- **The terminal.** tty detection, width, colour, paging, prompts, signal handling, exit codes.
- **Local resources the server cannot reach**: the workspace filesystem and local exec, served
  back over the existing reverse-channel. Note this is *mechanism*, not *policy* — the client
  serves file operations, it does not decide which commands need them.
- **Bootstrap that cannot require a server**: `aimee remote set`, `aimee version`, and the
  diagnostics that must work when no server is reachable. These must be explicitly enumerated
  and kept small, because every entry is a thing that can rot.

Interactive surfaces (`chat`, `launch`) already refuse a network endpoint because the agent and
worktree run on the client host. They are not in scope here and should stay as they are until
the reverse-channel covers them.

## §4 Versioning

One negotiated number: the client sends its wire version, the server answers with what it
speaks. A server too new for a client says so **in those words** — "this client speaks v1, this
server requires v2, upgrade the client" — rather than the current "has no /v1 route", which
misattributes a version problem to a typo.

This is what makes "no upgrade short of a large upgrade" true rather than aspirational: the
wire contract is the only thing that can force an upgrade, so it is the only thing to version.

## Phasing

1. **`POST /v1/cli` + argv forwarding**, with the existing route map retained as a fallback.
   Nothing is deleted yet; both paths work; the new one is proven against real commands.
2. **Server-side help/completion.** Delete `client_help[]` and `check-cli-help-coverage.py`.
3. **Delete the route map, marshallers and command table**, along with
   `gen-cli-v1-routes.py` and `check-cli-v1-routes.py`. This is the step that actually pays.
4. **Version negotiation** and the explicit bootstrap-command allowlist.

Each phase is independently shippable and each removes more than it adds after phase 1.

## Non-goals

- Changing what any command *does*.
- Moving interactive `chat`/`launch` execution off the client host.
- A plugin system for client-side commands. The point is fewer client-side commands.

## Risks / honest limits

- **Latency.** Today an unknown subcommand is rejected locally; server-side parsing costs a
  round trip to say "unknown command". Acceptable for a control-plane CLI, and it is the same
  round trip every *valid* command already pays — but it is a real regression for typos.
- **Offline behaviour gets worse, visibly.** Today some commands appear to work with no server.
  After this, they fail. That is more honest, but it is a behaviour change and the
  bootstrap allowlist (§3) is what keeps it tolerable. It must be decided deliberately, not
  discovered.
- **`POST /v1/cli` is a wide surface.** It accepts argv and executes it. It must sit behind the
  same capability checks as the routes it replaces, and must not become a way to reach a
  capability the registry would have denied. This is the main security review point.
- **stdin/streaming/exit codes** are the fiddly part of any argv-forwarding design and are
  where phase 1 will find its surprises.

## Tests

- A client built at commit N reaches a capability added to the server at commit N+1, with no
  client rebuild. This is the whole proposal in one test.
- `aimee help` against a newer server lists that server's commands.
- A capability the registry denies is still denied through `POST /v1/cli`.
- A client older than the wire contract gets an explicit upgrade message, not "no route".
- Bootstrap commands in the §3 allowlist work with no server reachable.

## Open questions

1. Does `POST /v1/cli` return a rendered presentation, or structured sections the client
   renders? Rendered is simpler and makes the client dumber; structured keeps `--json` honest
   and lets the client respect terminal width. Probably structured with a server-side default
   rendering.
2. Is the bootstrap allowlist (§3) fixed in the client, or does the client cache the last
   server manifest for offline use? A cache reintroduces staleness, but a fixed list
   reintroduces a table.
3. Do `chat`/`launch` eventually move behind the reverse-channel, or stay permanently
   client-local?
