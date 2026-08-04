# The aimee manual

aimee gives AI tools one memory, one code graph, controlled delegates, typed workflows, and a
server-side safety boundary. The CLI, browser, MCP, ACP, and compatible model APIs all reach the
same runtime.

Use the [Quickstart](docs/QUICKSTART.md) for installation. This manual covers normal use and
operations. Exact command and config tables are generated from source:

- [CLI commands](docs/gen/cli-commands.md)
- [Configuration](docs/gen/configuration.md)
- [HTTP routes](docs/gen/api-v1.md)

## The system

- `aimee` is a DB-free thin client. It runs hooks and stdio integrations, reads client files, and
  sends typed requests.
- `aimee-server` owns sessions, DB1, agents, tools, policy, credentials, provider calls, and the
  public resource API.
- `aimee-wfe` owns workflow definitions and lifecycle state.
- `aimee-kb` owns durable memory, documents, the code graph, retrieval, curation, PostgreSQL, and
  pgvector.
- Each KB owns its embedding and synthesis placements. A role can run inside that KB container or
  use a remote endpoint; there is no separate inference service.
- `aimee-runtime-web` serves the browser workspace.

The server and KB each run a bounded shared-memory event bus. Governed actions, memory mutations,
guardrail decisions, vault access, sandbox degradation, MCP activity, and tool outcomes pass through
one ordered audit tap. See [Event bus](docs/EVENT_BUS.md).

## First use

After the services are running, enroll the client and check every boundary:

```bash
aimee remote set https://host:8743 <wizard-bearer>
aimee remote status
aimee status
aimee kb status
aimee audit verify
```

Copy the command from the setup summary, then confirm the server certificate fingerprint out of
band. `remote set` stores the bearer, pins the server certificate, and on Linux enrolls a client
mTLS certificate. It does not rotate the bearer. Use `aimee remote enroll` when you need a separate
bearer for an additional client without invalidating existing clients.

Register a workspace from the machine that holds the files:

```bash
cd /path/to/project
aimee workspace add .
aimee index scan .
aimee index overview
```

A remote client uploads file content. A server never treats a client path as a path on the server.

## Memory

Use durable memory for facts that should survive tools and sessions:

```bash
aimee memory store infrastructure "Staging is in eu-west-1"
aimee memory search "where is staging"
aimee memory list
aimee memory get <id>
aimee memory read
```

The KB stores typed records with source, scope, confidence, freshness, and links to artifacts.
Curation joins duplicates, records contradictions, and lets stale evidence decay. Recall mixes
lexical, dense, graph, and recency signals, then may synthesize. A low-evidence query can
abstain instead of inventing an answer.

Working memory is session scratch:

```bash
aimee wm set branch feature/search
aimee wm get branch
aimee wm list
```

Do not use working memory for team facts. It is local, session-scoped DB1 state.

See [Knowledge](docs/KNOWLEDGE.md) and [Retrieval](docs/retrieval-stack.md).

## Code intelligence

```bash
aimee index find kb_client
aimee index structure src/server/server_state.c
aimee index callers kb_client_request
aimee index blast-radius src/modules/kb_client/kb_client.h
aimee graph explain <relationship>
```

The graph covers symbols, calls, imports, repository dependencies, and git co-change. In a
multi-repository workspace, caller and blast-radius results cross repository boundaries.

Run `aimee index scan` after large branch changes or when status says the index is stale. Use
`--force` only when incremental repair is not enough.

See [Code intelligence](docs/CODE_INTELLIGENCE.md).

The same index powers CSS migration checks. Use `aimee css report <project>` for an overview. The
optional render sidecar compares computed styles before and after a conversion.

## Delegates

A delegate is a cheaper or specialized agent working under server policy:

```bash
aimee delegate review --persona reviewer "Review the current diff"
aimee delegate diagnose --persona engineer "Find the cause of this failure"
aimee delegate code --persona engineer "Implement the accepted fix"
```

Roles describe the job. Personas describe the perspective and constraints. The router chooses a
viable agent that serves the role, respects concurrency and budget, and has the required tools. If
you pin an agent or model, failure is explicit; aimee does not silently substitute another one.

Read-only delegates may share the current tree. Write-capable delegates get an isolated worktree.
Container delegates default to no network and no ambient credentials. Package access, egress,
custom images, and toolchains are explicit policy.

Durable work is inspectable:

```bash
aimee jobs list
aimee jobs status <job-id>
aimee jobs logs <job-id>
aimee jobs cancel <job-id>
```

See [Delegates](docs/DELEGATES.md) and [Delegate sandbox](docs/DELEGATE_SANDBOX.md).

## Roundtables

Use a roundtable when one answer is not enough:

```bash
aimee ensemble roundtable --help
```

Seats run in parallel. Each seat has a persona and either a pinned model or a random eligible
reviewer. Reviewers must cite repository evidence. Failed random seats retry elsewhere; pinned seats
fail rather than changing identity. The optional chair removes unsupported or duplicate findings
and returns one result.

Roundtable cost belongs to the originating session or workflow. Set a positive cost cap if the run
must stop at a fixed spend.

See [Roundtables](docs/ENSEMBLE.md).

## Workflows

Control edges choose the next block. Typed input bindings choose the artifact that block reads. That
separation lets a graph loop through new work without feeding a gate its own stale verdict.

```bash
aimee workflow blocks
aimee workflow new ~/.config/aimee/workflows/change.yaml
aimee workflow validate ~/.config/aimee/workflows/change.yaml
aimee workflow show ~/.config/aimee/workflows/change.yaml
aimee workflow list
```

The Go control plane snapshots the definition and admitted request, then persists every lifecycle
transition before it dispatches work. Plans, code, reviews, and gate decisions remain separate
artifacts. A crash resumes from durable state.

Start a run directly or from a watched-proposal trigger:

```bash
aimee workflow run --help
aimee trigger fire --help
aimee trigger list
```

The current Go scanner executes `watch-dir` and `proposals` trigger sources. It records trigger mode,
but schedules `autonomous` and `interactive` runs the same way. A human gate parks until a browser or
API caller approves or rejects it. That decision is a hashed approval artifact and lifecycle
transition, not a cryptographic signature over the artifact and principal.

Parallel slices use separate branches and worktrees, then merge against the latest accepted feature
tip. A missing commit, merge conflict, exhausted loop, lost review replay, or forge failure parks or
fails with a named reason. An empty result never advances.

See [Workflows](docs/WORKFLOWS.md), [Workflow actions](docs/WORKFLOW_ACTIONS.md), and
[Autonomous development](docs/AUTONOMOUS_DEVELOPMENT.md).

## Agents, providers, and models

```bash
aimee agent list
aimee agent probe <name>
aimee provider list --available
aimee provider test <name>
aimee model list
aimee model show <model>
```

Provider payloads are translated into one canonical request/response IR. Policy, budgets, tool
handling, retries, accounting, and context reduction operate on that IR; provider-specific JSON
stays at the edge.

Credentials live in the server vault:

```bash
aimee vault unlock
aimee vault set <agent> <name> <secret>
aimee vault list
aimee vault lock
```

Do not put provider secrets in project files, delegate prompts, workflow artifacts, or client-side
agent registries. Local CLI agents may use their own login on the client when they execute there;
that credential is not uploaded.

## Personas, roles, rules, and toolsets

Personas set the point of view; role templates set the job contract:

```bash
aimee persona list
aimee persona show reviewer
aimee roles list
aimee roles show review
```

Rules are durable behavior constraints:

```bash
aimee rules list
aimee rules generate
aimee rules delete <id>
```

Skills are project-scoped instruction packages. Toolsets are named bundles of tools:

```bash
aimee skill list
aimee skill show <name>
aimee toolset list
aimee toolset resolve <name>
```

The host AI's own sub-agent launchers may be blocked so delegated work goes through aimee's policy,
worktree, budget, and audit path.

## MCP, ACP, and model ingress

`aimee mcp-serve` exposes memory, index, and delegate tools over stdio JSON-RPC. Client setup writes
the supported coding-tool registrations unless `AIMEE_NO_CLIENT_INTEGRATIONS=1` is set.

The MCP registry audits configured servers and checks packages against OSV:

```bash
aimee mcp audit
aimee mcp recheck
```

ACP editors can run the ACP bridge. OpenAI Chat Completions, OpenAI Responses, and Anthropic
Messages ingress let compatible clients use aimee as their model endpoint. All routes still pass
server auth, policy, provider translation, and audit.

Use `aimee api status` for endpoint snippets. See [Public API](docs/PUBLIC_API.md).

## Browser workspace

The browser includes chat, projects, agents, workflows, workflow actions, the code graph, logs,
settings, and per-user VS Code. Git credentials are stored in the server vault. SSH private keys
live in a sealed runtime area and are exposed to git through a short-lived agent, not copied into a
repository.

The managed deployment page can control the host Docker daemon because the server container mounts
its socket. Use the split stack when that authority is too broad.

See [Browser workspace](docs/DASHBOARD.md), [VS Code](docs/VSCODE.md), and
[Web git security](docs/WEBCHAT_GIT_SECURITY.md).

## Configuration

The main file is `~/.config/aimee/aimee.yaml`. Project settings live in
`.aimee/project.yaml`; multi-repository workspaces use `aimee.workspace.yaml`. Agents and network
inventory use `agents.json`.

```bash
aimee config show
aimee config get <key>
aimee config set <key> <value>
```

Environment variables override file values where the generated reference says they do. Restart-only
fields take effect on the next daemon start. The browser settings page exposes only allowlisted,
non-secret runtime fields.

Use the [generated configuration reference](docs/gen/configuration.md). It is more reliable than a
copied table in this manual.

## Remote access

`aimee remote set` stores the target in `~/.config/aimee/remote.conf`. That file is private to the
user and opened without following symlinks.

The shared bearer authorizes reads. The first wizard user's explicit `full` grant is bound to the
mTLS certificate enrolled by `aimee remote set`; the bearer alone cannot exercise it. Additional
users need a short-lived KB-signed identity token and a grant for the exact
`(server_id, team_id, subject)`. `data` covers memory, docs, and index ingestion. `full` also covers
agent, delegate, runner, and workspace control.

The server must know `AIMEE_SERVER_ID`, `AIMEE_SERVER_TEAM_ID`, and the root-owned
`AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE`. Grant changes are local-socket only:

```bash
aimee kb grant set --server <id> --team <n> --subject <subject> --tier data
aimee kb grant list --server <id> --team <n>
```

`aimee.api.remote_writes` remains readable for compatibility but no longer authorizes user writes.
See [Upgrading](docs/UPGRADING.md#restore-remote-writes) for subject forms and first-grant recovery.

Use `aimee remote clear` to return to the local Unix socket.

See [Thin client](docs/THIN_CLIENT.md) and [Security](docs/SECURITY.md).

## Audit and diagnostics

```bash
aimee status
aimee kb status
aimee workers
aimee audit verify
aimee doctor forensics
aimee economizer stats
```

The WORM audit ledger is the durable security record. Event-bus capture is the ordered diagnostic
and observational replay stream. Capture does not execute tools again.

Audit verification uses exit codes: `0` green, `1` amber, `2` red. Treat red as an integrity
failure, preserve the store, and investigate before rotating or truncating anything.

## Update and backup

Before an upgrade, back up:

- `~/.config/aimee/` including DB1, config, vault material, TLS state, and workflow files;
- the KB PostgreSQL database with `pg_dump` or the container export helper;
- any external witness or audit seal destination.

Do not copy a live SQLite file without its WAL/SHM files or a consistent backup operation. Do not
use `docker compose down -v` when a named volume is your only database copy.

After updating binaries and containers:

```bash
aimee server restart
aimee status
aimee kb status
aimee audit verify
```

On a remote thin client, `aimee self-update --check` compares the client with its server. Where
binary replacement is supported, `aimee self-update` downloads the matching release, verifies it,
and swaps the executable without downgrading.

Note: `aimee self-update --check` reports client and server build identifiers, but it cannot
order or auto-install non-semver branch builds. `aimee self-update --version vX.Y.Z` installs a
published semantic-version release and does not downgrade. Operators validating a `:testing` build
must install or build the matching testing thin client separately when exercising client-local catalog
behavior.

Read [What's new](docs/WHATS_NEW.md) before changing deployment manifests.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| client cannot connect | `aimee remote status`; URL, DNS, port, TLS fingerprint, bearer, client cert |
| local socket missing | service manager, `aimee server start`, server log, config-dir permissions |
| reads work but writes fail | server id/team/JWKS trust, exact subject grant, identity-token refusal reason |
| KB unavailable | `aimee kb status`, KB bearer, `AIMEE_KB_API_URL`, PostgreSQL readiness |
| memory search is empty | KB scope, ingest status, embedding readiness, query filters |
| delegate cannot write | assigned worktree, write role, sandbox backend, source authority |
| delegate has no network | expected default; configure mediated egress or packages explicitly |
| workflow parked | inspect the named reason, latest artifact, gate, agent limit, or merge conflict |
| audit count is short | event-bus drop counter, sink errors, unclean shutdown, capture file status |
| browser deploy fails | Docker socket mount, daemon access, image pull, volume permissions |
| model returns nothing | provider response diagnostic, model registry, auth, quota, retry record |

Logs live under `~/.config/aimee/` for source installs and in the relevant container for compose
deployments. Preserve the first error and the operation ID; later failures are often consequences.

## Files

```text
~/.config/aimee/
  aimee.yaml           main configuration
  agents.json          agents and network inventory
  aimee.db             DB1 SQLite
  remote.conf          thin-client target and trust state
  workflows/           workflow definitions
  captures/ or audit-* event capture and audit material
  tls/                 local certificate state
  server.log           server log

<project>/.aimee/project.yaml
<workspace>/aimee.workspace.yaml
```

DB2 is PostgreSQL, not a file under the config directory. Workflow lifecycle rows belong to the Go
control plane even when it shares the server container.

## Terms

| Term | Meaning |
| --- | --- |
| primary | the AI tool or model the user is working with |
| delegate | a policy-controlled agent doing a bounded task |
| persona | a named perspective and instruction set |
| DB1 | local server SQLite state |
| DB2 | KB PostgreSQL and pgvector state |
| event bus | intra-daemon typed shared-memory transport |
| capture | ordered observational bus record; never automatic execution replay |
| worktree | isolated git checkout assigned to a session or work item |
| gate | workflow step that requires a verdict before the run advances |
| IR | provider-neutral request and response representation |
