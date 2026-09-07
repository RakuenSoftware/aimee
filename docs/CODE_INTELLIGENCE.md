# Code intelligence

aimee stores code as symbols, references, calls, imports, repository dependencies, embeddings, and
git co-change. The graph lives in DB2 and can span every repository in a workspace.

## Local-first scope

Agent-facing code and knowledge searches default to the authenticated active project. The active
project is protected before candidate limits and ranking cutoffs, so a large global or stale corpus
cannot crowd it out. Missing project context returns the typed `scope_required` error; it never
widens silently. Use `scope=all` (or `"scope":"all"` in JSON) only for a deliberate cross-project
query. A project-scoped credential cannot request that broader scope.
For an unscoped owner, `project=<active>&scope=all` keeps the active project as the protected head
bucket and appends labeled results from other projects. This applies to symbol, text, caller, hybrid,
ranked knowledge/document, and typed-facet returns; each bucket is selected before the final limit.

Code requests may carry an observed `generation`. If it is no longer current, the request returns
`stale_generation` with the current generation instead of mixing old and new index data. Detached
projects are excluded from current queries.

## Index

```bash
aimee workspace add /path/to/repo
aimee index scan /path/to/repo
aimee index overview
```

A remote thin client reads and uploads source content. The server never scans a client path. Ingest
commits only after content, project, scope, and hash checks pass.

Supported tree-sitter extractors cover the languages listed by the running KB capability endpoint.
Do not rely on a copied language list; generated capabilities follow the built parsers.

## Query

```bash
aimee index find <name>
aimee index callers <symbol>
aimee index structure <file>
aimee index blast-radius <file>
aimee graph explain <relationship>
```

The CLI and installed MCP proxy derive the stable active-project identity from the request cwd.
Direct HTTP/MCP clients must send a stable `project` or explicit all-project scope. A checkout
basename is not a project identity and is never used as a fallback.

Caller and blast-radius results cross repository boundaries after dependencies have been resolved.
Vendored code, caches, generated trees, and configured excludes stay out of the project graph.

Blast radius resolves the target in the current project generation before returning an empty graph.
Python imports use dotted module identities (`app/dates.py` → `app.dates`), including explicit
`from app import dates`, package `__init__`, and imports relative to the importing file. Exact
normalized equality replaces path-substring matching. Direct call edges are included only when the
export is unique in the current project; cross-project edges require an existing structural route.
Each returned edge identifies its `provenance`, `confidence`, `project`, `generation`, and
`freshness`. Local current-project edges are emitted before any route-gated cross-project tail.

`ast_grep_search` is advertised only when its helper readiness contract passes. An unavailable helper
returns unsupported; it does not fall back to a different search and label it AST-aware.

## Hybrid code search

Text and vector matches find names and concepts. The call/import/dependency graph supplies structural
neighbors. Memory links add prior decisions and known constraints. Fusion keeps the evidence for each
signal so a client can explain the result.

## Live language-server commands

The durable index and the live LSP path answer different questions. Use the index to discover a
concept, history, or cross-repository relationship. Use LSP only when you already have an exact
file position and need the configured language server's definition or references.

Configure an operator-installed server in `aimee.yaml` on the host that runs the command:

```yaml
lsp_servers:
  - name: gopls
    command: /usr/local/bin/gopls
    extensions: [.go]
```

The current CLI accepts 1-based line and column positions:

```bash
aimee index lsp-def /absolute/worktree/main.go 8 9 --workspace /absolute/worktree
aimee index lsp-refs /absolute/worktree/main.go 8 9 --workspace /absolute/worktree
aimee index lsp-diag /absolute/worktree/main.go --workspace /absolute/worktree
```

`index lsp-rename` is an existing mutating expert command and is not exposed through the grouped
agent tool. Inspect its result and the worktree diff before retaining a change. MCP compatibility
calls use the grouped `lsp` commands `diagnostics`, `definition`, and `references`; their positions
are 0-based.

This is currently a prototype path with important limits:

- the caller supplies absolute workspace and file paths, so the process assumes those paths belong
  to the intended local or bind-mounted worktree;
- detached thin-client worktrees are not routed to the client that owns their files;
- the manager does not send document open, change, or close notifications and returns no content
  hash or document version, so freshness after a write is unproved;
- diagnostics reads stored notifications only and does not start a provider, so zero diagnostics
  with zero active servers is not proof of a clean worktree;
- failures are text shaped, and a crash and timeout can both appear as a request timeout;
- definitions encoded as LSP `LocationLink` are not parsed by the current client; and
- live providers are unsupported on Windows. Definition and reference calls say so, while the
  diagnostic call currently returns an indistinguishable empty result.

The S0 real-provider baseline under `benchmarks/live-semantic-context` pins and checks these facts
in pull requests. Do not describe this path as synchronized, sandboxed, detached-worktree safe, or
generally ready until the corresponding contract is promoted.

## Task-conditioned context

`GET /v1/code/context` is the strict, bounded ingress contract over the hybrid ranker. It requires
one active project, reads only its current generation, returns at most four code-plus-memory items,
and caps the rendered packet at 1,200 tokens. Exact lexical and structural evidence leads. A
vector-only result must clear the quality floor, and exact active-project memory is appended only
when it is linked to accepted code. Every code item carries project, path, generation, freshness, confidence,
provenance, and a line-or-file span.

A query without sufficient current-project code evidence returns HTTP 200 with
`status: abstained`, an `answerability.decision` of `no_answer`, empty `results`, and empty `why`.
It does not substitute global episodic
memory. The same local-first policy applies to the older hybrid memory annotations: project memory
is selected before workspace/shared memory and broad scope is never implicit.

Hybrid code responses always include `vector_status`: `disabled`, `ok`, `empty`, `stale`,
`unavailable`, or `unauthorized`. When the vector leg is stale, unavailable, or unauthorized,
dependency and dimension/retryability metadata are included; lexical and graph results remain usable
when they independently answer the query.

Ingress rollout is controlled by `code_context_mode`:

- `off` uses only the existing project-local preview path;
- `observe` retrieves and validates the packet, records its decision, and
  preserves existing model-visible bytes; and
- `on` (the shipping default after the E6 paired promotion gate) injects a packet only on the first
  turn of a session task or a low-overlap task change.

`on` does not repeat context for an ordinary follow-up and does not broaden after `no_answer` or an
unavailable KB. An unavailable first/new-task lookup rearms only its exact session/project marker,
so a related follow-up can use the dependency breaker's single recovery probe; successful,
genuinely empty, stale, and abstained results remain consumed. If the request working directory
cannot resolve a durable active-project identity, agent ingress suppresses code and memory recall
rather than issuing an unscoped query.

## Dependency status and recovery

Agent-facing retrieval preserves six outcomes: `ok`, `empty`, `abstained`, `stale`, `unavailable`,
and `unauthorized`. `empty` is emitted only after a valid response; an outage or malformed response
is never converted to an empty list. Stale results carry the observed/current generation or vector
dimension when available. Unavailable results name the failed dependency, say whether retry is
safe, and include a bounded retry delay.

The server-side KB client and the KB-side external embedder each use a process-local circuit
breaker. Three consecutive transient failures open it with bounded exponential backoff and jitter.
Calls during the delay are suppressed, then exactly one half-open recovery probe is admitted.
Success closes the breaker without restarting the client. A reachable KB reporting its own
embedder or vector-store outage does not open the KB transport breaker, so unrelated KB operations
remain usable. Built-in local embeddings bypass the external dependency breaker.

Local inspection, editing, and tests do not depend on KB recovery. Clients may fall back to those
local operations on `unavailable`, but must not silently retry without a bound or report an
Aimee-assisted result for the failed turn.

## Audits

```bash
aimee code audit <dir>
aimee code audit --graph --project <name>
```

The local audit reports file-level health. The graph audit reports dead exports, import cycles, exact
clones, and near clones from an indexed project. `--fix` is non-mutating unless the installed help
explicitly says otherwise.

## CSS migration

The CSS index records selectors, declarations, variables, conflicts, component use, and migration
units:

```bash
aimee css report <project>
aimee css dead-rules <project>
aimee css conflicts <project>
```

Computed-style verification uses an isolated Chromium sidecar. Capture a before and after snapshot,
then compare them with `aimee css render-verify`. See the
[CSS render sidecar](../deploy/css-render/README.md).

## Refresh and failure behavior

Incremental scan updates changed content. Use `--force` after extractor/schema changes or when a
repair says the generation is inconsistent.

An empty scan cannot mark a previously populated project healthy without recording why no files were
seen. Remote index writes require a data grant for the authenticated subject.

Before a broad edit, query callers and blast radius, then inspect the named files. The graph narrows
work; it does not replace tests.

## Project generations and lifecycle

Moving or re-adding a checkout updates its alias and retains one stable project identity. A new
generation is created only when a detached project is re-added; default queries read the current
generation only.

One lifecycle operation ships today:

```bash
aimee workspace remove /path/to/repo       # unregister only; preserves indexed data
```

**`index detach`, `index purge`, and `index gc` are designed and not shipped.** `aimee index` has no
such subcommands, and no `/v1` route deletes an index. The contract below is the accepted design
from [agent-facing code intelligence](proposals/done/agent-facing-code-intelligence-effectiveness.md),
kept here so an operator planning retention knows what it will require. Until it lands, indexed data
outlives `workspace remove` and no supported command deletes it.

Purge and garbage collection will be dry-run by default. Their manifest includes the stable project,
generation, policy criteria, per-table counts, and SHA-256 fingerprints of the exact physical
targets. Confirmation recomputes that manifest in a serializable transaction and fails if any row
or criterion changed. A confirmed mutation requires an authenticated unscoped owner, derives the
principal from that verified request context, and records the principal, project, generation,
timestamp, reason, criteria, manifest hash, and counts. If audit commit fails, deletion is refused.
