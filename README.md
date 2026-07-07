# aimee

aimee has two parts.

**aimee-kb** is a general purpose knowledge base for a whole corpus of knowledge, whether
that's a specific genre of knowledge, a company knowledge base, or a team's knowledge base.

**aimee-server** is an assistant to a human. It learns how to work best with that human,
learns and understands their expectations, and follows them. It is a general purpose
assistant. It started as a coding assistant, and coding is still what it does best. It is
*very strong* there, especially on multi-repo and large-repo work.

**Your AI has no memory, no map of your work, and no brakes. Every session it starts blind,
bills you to catch it up again, and can still overwrite your `.env`.**

**aimee fixes all of it, and goes further.** One memory across every tool. Your knowledge and
your code as one live graph. Any model, any provider. Cheap delegates for the grunt work.
Guardrails it cannot write past. Workflows that run a whole job start to finish, with review
panels and your sign-off built in. Your context follows you anywhere. Nothing locks you in.

The memory and the code index are a **hybrid vector-graph**: vector recall fused with a typed
knowledge graph and your code's call graph, ranked together. That is why it surfaces what the
plain vector search behind most AI memory misses, the caller three files away, the decision
from another session, the constraint that never shared a keyword with your query.

Point any tool's OpenAI or Anthropic compatible API at aimee and it runs the turn on any
model reachable over that wire: Claude, GPT, Gemini, Mistral, MiniMax, a model on your own
GPU, or any other OpenAI or Anthropic compatible provider. Or run aimee beside your tool over
MCP, ACP, or a plugin, where the tool keeps its own model and aimee adds the memory,
delegates, and guardrails as tools and hooks. If it speaks the Anthropic or OpenAI API, MCP,
or ACP, it works: Claude Code, Codex, OpenCode, Gemini CLI, aimee's own webchat, and anything
else on those protocols. Switch tools whenever you like. Your memory and context come with
you.

Core services are C, hot paths run in single digit milliseconds, and nothing phones home.

## What you get

**Memory that compounds.** Every session starts already knowing what the last one learned.
A curator pipeline distills raw sessions into a typed knowledge base, dedupes facts, flags
contradictions, and lets stale detail decay. Point a team at one shared kb and everyone
works from the same institutional memory. See [How aimee learns](docs/KNOWLEDGE.md).

**Your codebase as a map.** aimee indexes your code into a symbol and call graph. The AI
finds callers, traces an edit's blast radius before it writes a line, and works from the
graph instead of reading your files over again each session. The graph spans repositories, so one
dependency map covers every repo you work across.

**Delegates that cut the bill.** Send summarization, review, and boilerplate to the cheapest
model that can handle it. Local models on your GPU cost nothing. Subscription delegates like
ChatGPT Plus or a Mistral plan cost nothing extra. The primary agent gets a compact result
back instead of raw content, so you save twice, on the delegate and on the primary's
context. aimee also compresses what each turn sends upstream, so even your main model's bill
drops. It tracks cost and success per delegate and routes better over time.

**Tokens cut on both ends.** A context economizer works in two tiers. The safe tier
runs by default: it deterministically condenses tool output (keeps the failing test,
elides the passing ones; keeps compiler errors, drops progress spam), compresses oversized
results, and folds stale turn history into a rolling skeleton, with the full output spilled
to disk for recovery. The aggressive tier is opt in: it applies the same reduction to the
live primary request so your main model's own bill drops too, compress only, behind a per
session circuit breaker that never dispatches anything it cannot restore. See
[Settings](docs/SETTINGS.md).

**A tamper evident record of every action.** Each governed tool call runs through one choke
point that decides allow, rewrite, or block, and writes that verdict to an append only,
mode-0600, rotated audit ledger: who acted, which tool, the mode, a stable reason code, and
a keyed HMAC digest of the arguments, keyed so a low entropy argument cannot be recovered
from the log and allowlisted so a new tool can never leak a secret into it. Human sign off
gates are HMAC-SHA256 signed and non forgeable. Decision records capture what you decided,
why, what it supersedes, and when to revisit it, one active per scope. See
[Security Model](docs/SECURITY.md) and [KB Console](docs/KB_CONSOLE.md).

**Run the models yourself.** aimee ships a self hosted inference stack: embeddings,
reranking, and synthesis baked into one CPU or GPU container. The knowledge base curates entirely
on your hardware with no outside API calls, and the same local model doubles as a free
delegate. Swap the model tier with a single image, or run the CPU build for retrieval on any
box. See [kb LLM backends](docs/KB_LLM_BACKENDS.md).

**Workflows.** Compose a job out of typed steps and aimee runs it start to finish: the
delegates do the work, review panels check it, and it parks at a human gate wherever you want
the final say. The `build` workflow ships as the default, taking a written proposal all the
way to a PR, and you can edit it or author your own. See [Workflows](docs/WORKFLOWS.md) and
[Autonomous Development](docs/AUTONOMOUS_DEVELOPMENT.md).

**Guardrails and isolation.** Sensitive files like `.env`, private keys, and production
configs are blocked before the AI can touch them. Known anti patterns raise warnings.
Planning mode freezes every write until you are ready. Each session runs in its own git
worktree, so two sessions never step on each other.

**A browser workspace.** aimee ships its own browser UI, `aimee-webchat`:
chat, a live view of your code as a graph, a git project manager that clones repos and holds
per host tokens, a full in app VS Code editor, and dashboards over what the server is doing.
No terminal required. See [Dashboard & Logs](docs/DASHBOARD.md).

**Ready for a team.** Multi user accounts with scoped tokens, OIDC single sign on, optional
mutual TLS client identity, and a per principal encrypted credential vault so one user's git
token or provider key is never readable by another. See [Security Model](docs/SECURITY.md).

**Documents become evidence.** Ingest a PDF and every retrieved snippet carries the page and
bounding box it came from, so an answer traces back to the exact spot on the page. Table
cells, figure crops, and OCR of scanned pages layer on top. See
[Structured PDF](docs/STRUCTURED_PDF.md).

**A roundtable of models.** Fan a task out to a panel and synthesize one answer, or run a
bounded multi round draft-or-review where several models critique and converge. The same
mechanism staffs the reviews in autonomous development. See [Personas](docs/personas.md).

## The problem

Every AI session starts blank. Your tool does not remember your infrastructure, your
preferences, your past mistakes, or what it did five minutes ago in another window. You
spend tokens explaining context again, mapping your own codebase again, and correcting the
same errors over and over.

And nothing stops it from overwriting your `.env`, editing a production config, or
clobbering another session's work.

## What aimee does

aimee runs as a local server your tools connect to. In the path, it runs your turns over the
OpenAI or Anthropic compatible ingress. Beside your tool, it intercepts actions through
hooks, MCP, ACP, and plugins. The CLI is `aimee`. Browser chat is `aimee-webchat`.

```mermaid
graph TB
    User["You"]
    PA["Primary Agent<br/>Claude Code / Codex / OpenCode / Gemini CLI / Vibe"]
    Hooks["aimee hooks<br/>SessionStart, PreToolUse, PostToolUse"]
    Memory[("Memory<br/>four tier, scoped search")]
    Guard["Guardrails<br/>Sensitive file blocking<br/>Anti pattern detection<br/>Planning mode"]
    Router["Delegate Router"]

    D1["Ollama<br/>local, free"]
    D2["ChatGPT Plus<br/>subscription, free"]
    D3["Claude API<br/>pay per token"]

    User --> PA
    PA <--> Hooks
    Hooks --> Memory
    Hooks --> Guard
    PA -->|"aimee delegate"| Router
    Router --> D1
    Router --> D2
    Router --> D3
```

### Memory and a knowledge base that learns

A four tier memory system tracks project context, infrastructure, preferences, and past
outcomes. Facts are deduplicated, contradictions are detected, and stale information decays.
Every session starts already knowing what matters.

A curator pipeline extracts, synthesizes, judges, and promotes that into a typed graph, and
reflects on idle time to improve it. Point a team at a shared `aimee-kb` and the whole team
shares one knowledge base, across every domain. See [How aimee learns](docs/KNOWLEDGE.md).

### Code intelligence

aimee builds a searchable symbol and call graph of your code, so the AI can find every
caller of a function, trace what an edit will touch before it writes, and answer from the
graph instead of reading the files over again. The graph is cross repo: one dependency map spans every
repository you work across, so a change in a shared library shows its blast radius in the
services that consume it. See [Code intelligence](docs/CODE_INTELLIGENCE.md).

### Documents as coordinate anchored evidence

aimee-kb can ingest a PDF as coordinate anchored evidence instead of a flat blob of text.
Every retrieved snippet carries the page number and bounding box it came from, so an answer
is traceable to the exact place on the page rather than to an unlocatable paraphrase. On top
of that spine the KB optionally recognises table cells (structured `{row, col, text}`
lookups), renders visual crops of figures, tables, and pages into a content addressed store,
and OCRs scanned or image only pages back through the same citation path. Each layer is its
own opt in and degrades to the one below when its dependency is absent, and every
read surface honours the same document level access control as the text spine. See
[Structured PDF](docs/STRUCTURED_PDF.md).

### Guardrails

Before every edit, aimee classifies the target. Sensitive files like `.env`, credentials,
and private keys are blocked before the AI touches them. Known anti patterns trigger
warnings. Planning mode locks all writes until you are ready.

### Governance and a tamper evident action ledger

Every governed tool call passes through one choke point (`pre_tool_check`) that computes an
allow, rewrite, or block verdict and records that verdict to a single append only,
mode-0600, rotated audit ledger: `{ts, actor, tool, mode, reason_code, verdict, args_hash}`.
The `reason_code` is a stable event key, never free form prose, so nothing user bearing is
persisted. `args_hash` is a keyed HMAC-SHA256 over a per tool allowlist projection of the
arguments: keyed so low entropy arguments cannot be recovered from the public log by
dictionary attack, allowlisted so a new tool or a new argument can never
silently leak a secret or PII value into an append only log, and versioned. Auditing is
fail open: the log write happens after the verdict is decided, so a
logging failure can never flip an allow to a block. The ledger is replayable:
`trajectory_export` interleaves the governed actions back into the session timeline by
timestamp, and the operator console exposes the feed at `/v1/audit/actions`.

Alongside the action ledger, aimee keeps **decision records**: governable "we decided X
because Y" entries carrying status, rationale, alternatives, a revisit date, what they
supersede, the author, and the policy they bind. At most one decision is active per scope
(enforced by a database unique index, not just by the writer), superseding one flips the
prior in the same transaction, and a decision past its revisit date resurfaces on the
existing recall and curator sweeps rather than rotting silently. Human sign off gates on
sensitive actions are HMAC-SHA256 signed and non forgeable. Both surfaces are live in the
[KB Console](docs/KB_CONSOLE.md) (`/v1/decisions`, `/v1/audit/actions`).

Retrieval has a parallel, opt in audit chain: every KB grounded answer is reconstructible.
Read back which sources grounded it, at which content hashed version, and whether the answer
is entailed by that evidence, through `/v1/audit/{trace,provenance,fidelity}`.

### The context economizer

aimee runs a context economizer over both the delegate turn loop and, optionally, the live
primary `/v1` path, gated by a master switch and split into two tiers. The **safe tier**
is default on: deterministic tool output condensation (keep the failing test, elide the
passing ones; keep compiler diagnostics, drop progress), size based compression of oversized
tool results, and folding old turn history into a rolling skeleton. Recent context is never
touched, and the full output is spilled to disk for recovery. A measurement ledger records
the reduction opportunity even when a lever is off, so the master switch off state provably
reduces to zero.

The **aggressive tier** is opt in and off by default: it applies the reduction to the live
inbound request so the primary agent's own tokens shrink too. It is compress only, and
guarded by a per session circuit breaker. If a reduced request draws an error, the session
falls back to the pristine payload and disables reduction for its remaining turns, so one bad
reduction can never persistently break live traffic. aimee never dispatches a reduced payload
it cannot restore to the byte identical original. See [Settings](docs/SETTINGS.md) and
[Tool output condensation](docs/features/tool-output-condensation.md).

### Delegation that cuts your bill

Route summarization, formatting, review, and boilerplate to cheaper models. The primary
agent gets a compact result instead of raw content, so you save on the delegate and on the
primary's context. Local models (Ollama, or aimee's own GPU synth) cost nothing.
Subscription delegates (ChatGPT Plus, a Mistral plan) cost nothing extra. The router picks
the cheapest delegate that can do the job, and tracks cost and success per delegate so
routing improves over time. Every turn is also compressed on the way upstream, which trims
the primary model's bill on top of the delegate savings.

```bash
aimee delegate review "Review this PR"     # routes to cheapest capable delegate
aimee delegate code --tools "Add tests"    # delegate with file read/write access
```

When one model is not enough, run a **roundtable**: fan a task out to a panel and have an
aggregator synthesize a single answer (`aimee delegate aggregate`), or run a bounded
multi round draft-or-review where several models critique each other and converge, with cost
and round caps (`aimee delegate roundtable --mode draft|review`). This is the same panel that
staffs the reviews in autonomous development, and it composes from your configured
[personas](docs/personas.md).

```bash
aimee delegate aggregate "Design an idempotent retry scheme"     # Mixture-of-Agents fan-out + synthesis
aimee delegate roundtable "Review this design" --mode review     # bounded multi-round critique
```

### Run your own inference

aimee ships the retrieval and synthesis stack as one container: a Vulkan llama.cpp runtime
serving embeddings, reranking, and synthesis from models baked into the image. Run it on a
GPU and the knowledge base curates locally with no API calls, and the local model registers
as a free delegate the roundtable and the primary can call. Four tiers cover the range: a
CPU build for retrieval on any host, and three GPU builds (for 16, 24, and 32 GB cards) whose
synth model you pick with a single image swap, no re-embed. See [kb LLM backends](docs/KB_LLM_BACKENDS.md) and
[synth tiers](docs/AIMEE_KB_SYNTH_TIERS.md).

### Session isolation

Each session gets its own git worktree, state file, and branch. Run two in parallel and they
never clobber each other. Read only delegates inspect the parent worktree directly. Write
capable delegates get isolated sibling worktrees.

### Any model behind your front end

aimee-server speaks the same wire formats your tools use, so you can point a tool's front end
at aimee and run every turn on aimee's configured primary model instead of the tool's built
in vendor. The tool keeps owning its prompt, history, and tools. aimee translates the wire
format and swaps the model. Switch with `aimee primary <agent>`. `GET /v1/models` lists what
is selectable.

```bash
# Claude Code on any model (Anthropic Messages ingress):
aimee claude-proxy enable http://127.0.0.1:8910 <server-bearer>
ANTHROPIC_BASE_URL=http://127.0.0.1:8910 ANTHROPIC_AUTH_TOKEN=<bearer> claude
```

Codex (OpenAI Responses), OpenCode, and any OpenAI compatible client work the same way.
Provider config is in the
[Technical Reference](src/README.md#deployment-and-operations) and the
[Manual](MANUAL.md#25-integrations). The memory, guardrails, and delegation are the same on
every front end, because they live in the server and kb, not the tool.

### A browser workspace

aimee ships its own browser UI, `aimee-webchat`, so you never need a terminal to use it. It
is a thin, stateless client that proxies to `aimee-server`, with one tab per tool: **Chat**
against any configured model; a **Graph** explorer that renders your indexed code as a live
symbol and call graph; **Projects** to clone git repos over HTTPS or SSH form URLs; a full
in app **VS Code editor** (a per user `code-server` the server supervises and reverse proxies)
opened on the selected project; **Workflow Actions** to author a proposal and watch it run
autonomously; and a **Dashboard** plus a **Logs** tab that show the server's delegations, tool
calls, token spend, guardrail verdicts, and active sessions at a glance. See
[Dashboard & Logs](docs/DASHBOARD.md) and [Workspace Management](docs/WORKSPACES.md).

### Built for a team

aimee is single user on a laptop and multi user on a server without changing the model.
Clients enroll as accounts with **scoped tokens** (a request outside a token's grant is
denied with `403`); the browser console authenticates with **OIDC single sign on**; the
remote `/v1` path can require **mutual TLS** client identity; and every user's git tokens,
provider keys, and OAuth credentials live only in a **per principal encrypted vault**, sealed
by a server master key so one tenant can never read another's secret or files. The vault's
master key rotates through an operator gated, offline runbook. See
[Security Model](docs/SECURITY.md), [KB Console](docs/KB_CONSOLE.md), and
[Webchat git security](docs/WEBCHAT_GIT_SECURITY.md).

## Works with what you already use

| Tool | Integration | Run on any model | Setup |
|------|------------|------------------|-------|
| Claude Code | Full hook support, MCP | Yes, via the Anthropic ingress (`/v1/messages`) | `./install.sh` or `aimee claude-proxy enable` |
| Codex CLI | Full hook support, MCP, local plugin | Yes, via the OpenAI Responses ingress (`/v1/responses`) | `./install.sh` |
| OpenCode | TUI front end (`opencode attach`) | Yes, via the OpenAI compatible ingress | `./install.sh` |
| Gemini CLI | Full hook support | Provider CLI | `./install.sh` |
| Mistral Vibe | Provider CLI primary and subscription plan delegates | Provider CLI | `aimee agent add <name> <endpoint> <model> --provider mistral` |
| GitHub Copilot | MCP server | Via the OpenAI compatible model | `./install.sh` |
| VS Code | MCP tools in Copilot Chat, an ACP agent (`aimee acp-serve`), or aimee as an OpenAI compatible model | Yes, via `/v1/chat/completions` or ACP | [VS Code guide](docs/VSCODE.md) |

These are the common ones. Anything that speaks the Anthropic or OpenAI API, MCP, or ACP
works the same way. Switch tools any time. Your memory and context stay.

## Why use aimee

- **Never start from zero.** Memory and context persist across sessions and tools, and
  compound into a knowledge base.
- **No lock-in.** Run any turn on any provider's model, or your own, and switch with one
  command.
- **Lower bills.** Cheapest capable delegate routing keeps the primary's context small, the
  context economizer trims every turn on both the delegate and the primary path, and local
  and subscription delegates cost nothing extra.
- **Fewer mistakes.** Sensitive file blocking, anti pattern warnings, and planning mode
  catch problems before the AI writes them.
- **Provable and governable.** Every governed action lands in a tamper evident, keyed-HMAC
  audit ledger, decisions are recorded with rationale and a revisit trigger, and KB grounded
  answers are reconstructible back to their sources.
- **Team knowledge.** One shared kb distills what your whole organization knows, behind
  scoped accounts, OIDC sign on, and a per principal encrypted vault.
- **A UI when you want one.** A browser workspace with chat, a code graph explorer, git
  project management, an in app VS Code editor, and server dashboards. No terminal required.
- **Yours to run.** C services, single digit millisecond hot paths, and an inference stack
  you can run entirely on your own hardware.

## Get started

aimee is a few services plus a thin client. Run the services in Docker (one combined
container) and install only the `aimee` CLI on each developer machine, pointed at the
server.

```bash
# 1. Run the stack (server, kb, Postgres, embedder)
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.combined.yaml up --build -d

# 2. Confirm it's live (default bearer: aimee-local-dev; -k accepts the self signed cert)
curl -k -H 'Authorization: Bearer aimee-local-dev' https://localhost:8743/v1/health
```

Then install the thin client (prebuilt Linux, macOS, and Windows binaries ship with each
release), point it at the server with `aimee remote set https://host:8743 <token>` (the
server's `/v1` is TLS only off loopback with a self signed cert, so set `AIMEE_TLS_INSECURE=1`
or trust the cert), and register your tool's hooks with `./configure-hooks.sh`.

The [Quickstart](docs/QUICKSTART.md) walks the Docker server and thin client setup step by
step. Every deployment topology, the from source install, remote operation, scaling, and
performance live in [Deployment and operations](src/README.md#deployment-and-operations).

## Quick start

```bash
# Store a fact the AI remembers across sessions
aimee memory store myhost "PVE at 10.0.0.1" --tier L2 --kind fact

# Search memory
aimee memory search "proxmox"

# Delegate routine work to a cheaper model
aimee delegate review "Review this PR for security issues"

# Scratch state for the current session
aimee wm set current-task "Review PR for security issues"

# Health
aimee status
```

## Documentation

| Document | Description |
|----------|-------------|
| [Quickstart](docs/QUICKSTART.md) | Run the combined server in Docker, then install the Linux, Windows, or macOS thin client. |
| [How aimee learns](docs/KNOWLEDGE.md) | The knowledge base, self learning pipeline, cross domain synthesis, and delegation economics. |
| [kb LLM backends](docs/KB_LLM_BACKENDS.md) | Pointing aimee-kb at its embedding, reranking, and synthesis backend (an `aimee-kb` tier image or an external endpoint), the config surface, and the tiers and dims. |
| [Synth tiers](docs/AIMEE_KB_SYNTH_TIERS.md) | The three self hosted inference tiers, the plugin image swap, GPU runtime requirements, and the synth tuning knobs. |
| [Manual](MANUAL.md) | Full user reference: install, config, command contract, feature guides. |
| [Architecture](docs/ARCHITECTURE.md) | Processes, storage and trust boundaries, layering, request lifecycles. |
| [Technical Reference](src/README.md) | Code internals, module map, server internals, RPC and HTTP surfaces, build system, and deployment and operations. |

Focused references:

| Document | Description |
|----------|-------------|
| [Command Reference](docs/COMMANDS.md) | Client command contract, flags, and options |
| [Storage Tiers](docs/STORAGE_TIERS.md) | DB1 and DB2 ownership boundaries (pgvector inside DB2) |
| [Structured PDF](docs/STRUCTURED_PDF.md) | Coordinate anchored PDF evidence: text and geometry citations, table cells, visual crops, and OCR. The capability flags, retrieval surface, and access model. |
| [Code intelligence](docs/CODE_INTELLIGENCE.md) | The symbol and call graph, cross-repo dependencies, blast radius, graph audits, and how the AI queries it |
| [Setting Up Delegates](docs/DELEGATES.md) | Configure delegate agents for task offloading |
| [Autonomous Development](docs/AUTONOMOUS_DEVELOPMENT.md) | Hand aimee a proposal and it builds the change end to end via the workflow engine |
| [Personas](docs/personas.md) | Built in and custom agent identities, delegate policy, and how personas staff reviews |
| [Workflows](docs/WORKFLOWS.md) | The composable dev lifecycle workflow engine, block catalog, and authoring |
| [Workflow Actions](docs/WORKFLOW_ACTIONS.md) | The web page to author a proposal, run it autonomously, and watch its status/history |
| [Dashboard & Logs](docs/DASHBOARD.md) | The web Dashboard's server-incurred metric panels, customization, the Logs (tool-action audit) tab, and the panel data architecture |
| [Settings](docs/SETTINGS.md) | The web page for the server's typed runtime config: economizer levers, autonomous-dev knobs, tool-output condensation, and how each maps to `aimee.yaml` |
| [Context economizer](docs/features/context-fold.md) | The two-tier token-reduction pipeline: history fold, tool-output condensation, size compression, the freeze cost guardrail, and the live-primary gateway mutation with its per-session circuit breaker |
| [KB Console](docs/KB_CONSOLE.md) | The operator console's trust model and surfaces: Accounts (enroll/revoke/scopes/OIDC) and Governance (decision records, the policy-verdict action audit) |
| [Workspace Management](docs/WORKSPACES.md) | Multi repo workspaces and session isolation |
| [Security Model](docs/SECURITY.md) | Threat model, trust boundaries, capability system |
| [Webchat git security](docs/WEBCHAT_GIT_SECURITY.md) | How webchat handles a webuser's git forge token at rest, in transit to git, and in the in browser editor, and where exposure is and is not closed |
| [Benchmarks](docs/BENCHMARKS.md) | Latency measurements and performance budget |
| [Compatibility](docs/COMPATIBILITY.md) | Supported OS, shell, and provider matrix |
| [VS Code Integration](docs/VSCODE.md) | Wire aimee into VS Code via MCP tools or as an OpenAI compatible model |
| [Feature Status](docs/STATUS.md) | Implementation status of all features |

## Community

Questions, help, and discussion happen on the **official aimee Discord**:
<https://discord.gg/FjGjvcgAqz>.

## License

aimee is Copyright (C) 2026 The aimee authors and is licensed under the **GNU Affero General
Public License v3.0** (AGPL-3.0). See [LICENSE](LICENSE) and [NOTICE](NOTICE).

If the AGPL doesn't suit you, other licensing can be discussed. For commercial or alternative
terms, contact the aimee authors at <jbailes@gmail.com>.

Bundled third party components (cJSON and the generated SDKs under `api/sdks/`) are licensed
separately. See [NOTICE](NOTICE).
