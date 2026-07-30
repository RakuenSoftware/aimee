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

`ast_grep_search` is advertised only when its helper readiness contract passes. An unavailable helper
returns unsupported; it does not fall back to a different search and label it AST-aware.

## Hybrid code search

Text and vector matches find names and concepts. The call/import/dependency graph supplies structural
neighbors. Memory links add prior decisions and known constraints. Fusion keeps the evidence for each
signal so a client can explain the result.

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

Lifecycle operations are intentionally distinct:

```bash
aimee workspace remove /path/to/repo       # unregister only; preserves indexed data
aimee index detach stable-project-id        # hide current generation; preserve data
aimee index purge stable-project-id         # read-only exact-target manifest
aimee index purge stable-project-id \
  --confirm <manifest-sha256> --reason '<reason>'
aimee index gc [stable-project-id] --retention-days 30
aimee index gc [stable-project-id] --retention-days 30 \
  --confirm <manifest-sha256> --reason '<reason>'
```

Purge and garbage collection are dry-run by default. Their manifest includes the stable project,
generation, policy criteria, per-table counts, and SHA-256 fingerprints of the exact physical
targets. Confirmation recomputes that manifest in a serializable transaction and fails if any row
or criterion changed. A confirmed mutation requires an authenticated unscoped owner, derives the
principal from that verified request context, and records the principal, project, generation,
timestamp, reason, criteria, manifest hash, and counts. If audit commit fails, deletion is refused.
