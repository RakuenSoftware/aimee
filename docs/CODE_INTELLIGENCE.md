# Code intelligence

aimee stores code as symbols, references, calls, imports, repository dependencies, embeddings, and
git co-change. The graph lives in DB2 and can span every repository in a workspace.

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
