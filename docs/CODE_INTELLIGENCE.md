# Code intelligence

aimee indexes your code into a symbol and call graph and keeps it on the server, so the AI
works from the graph instead of reading your files over again every session. It finds every
caller of a function, traces what an edit will touch before it writes, and pulls the exact
span it needs instead of loading whole files. The graph spans repositories, so one dependency
map covers every repo you work across.

## What it indexes

aimee extracts symbols, references, and call edges with tree-sitter across C, C++, C#,
Python, Go, JavaScript, TypeScript, Rust, Java, Ruby, PHP, Kotlin, Swift, Dart, Lua, Bash,
and CSS. The graph and the code spans live in DB2 (Postgres with pgvector), next to the
knowledge base, so a query is a database lookup, not a filesystem scan.

## Build and refresh the index

```bash
aimee index scan            # scan your workspaces and build or refresh the graph
aimee index scan --force    # rebuild from scratch
aimee index list            # list indexed projects (alias: index overview)
```

A workspace you add with `aimee workspace add` is indexed as part of the same graph. See
[Workspace management](WORKSPACES.md).

## Query it

```bash
aimee index find <name>            # locate a symbol or identifier
aimee index callers <symbol>       # who calls this
aimee index blast-radius <file>    # files a change here would touch
aimee index structure <file>       # the symbols in a file
```

## Cross-repo dependencies

The graph is not per repo. aimee resolves dependency edges across the repositories in your
workspace, so a change in a shared library shows its blast radius in the services that consume
it, and a caller lookup crosses repo boundaries. You can ask in three directions: `out` for
what a project depends on, `in` for the repos that depend on it (the reverse map), or `both`.
Vendored trees and dependency caches are skipped so they do not pollute the graph.

## Graph audits

`aimee code audit --graph` runs graph-derived health checks on an indexed project and returns
dead exports, import cycles, exact clones, and near clones.

```bash
aimee code audit --graph [--project P] [--json]
```

A thin client needs a configured remote and an indexed project for this. Plain
`aimee code audit [dir]` runs local file-health checks without the graph.

## How the AI uses it

The graph is a tool surface for the primary agent and its delegates, not only a CLI. Before
an edit the AI looks up callers and blast radius, and it fetches exact code spans by symbol
instead of loading whole files into context. That is what lets it work from your code, keep
its context small, and stop rediscovering the codebase on every session. Code search is a
hybrid vector-graph like memory: the call graph, vector similarity, and the cross-session
knowledge graph are ranked together (`/v1/code/hybrid`, tuned by `code_hybrid_weight_graph`
and `code_hybrid_weight_memory`), so a lookup follows structure and meaning, not just text.
See [How aimee learns](KNOWLEDGE.md) for how this sits alongside memory and the curator.
