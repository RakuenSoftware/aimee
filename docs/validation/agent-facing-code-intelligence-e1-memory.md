# Agent-facing code-intelligence E1-memory validation

- **Slice:** E1-memory. Local-first memory returns
- **Based on:** E1 merge `a8d3214c2d057fa95820e6acb60b788e403d7c68`
- **Policy authority:** local-first amendment merge
  `e9626cda560b7e9b1bbf96e48c05c94b62a72c8a`
- **Untreated control:**
  [`agent-facing-code-intelligence-red-baseline.md`](agent-facing-code-intelligence-red-baseline.md)
- **Environment:** Debian, SQLite DB2 test backend; Postgres/pgvector remains a CI integration gate

## Treatment

E1-memory resolves project and workspace identity once at the agent-facing request boundary and
propagates it through the server, KB client, KB service, and DB2 retrieval layers. Ordered memory
returns now use one protected bucket order:

1. active project;
2. active workspace;
3. shared/global (including untagged compatibility rows); and
4. other projects only for explicit `scope:"all"` requests.

Scope filtering and bucket ordering happen before SQL limits, semantic rerank cutoffs, result caps,
and context token assembly. Relevance, confidence, and freshness retain their existing order within
each bucket. When active context is missing, the response is labeled `active_context_missing` and
only shared/global rows are eligible. Exact-ID, exact-key, and explicit exact-scope operations retain
their direct compatibility semantics.

The policy is shared by fact and semantic search, list/candidate readers, answer evidence, graph and
entity relations, episode search, recall/context assembly, lifecycle alerts, and briefing
facts/activity/entities.
The native MCP memory tools, `/v1/memory/recall`, server state reads, CLI memory commands,
session-start/turn/compact recall, and agent-runtime context build all supply the request-local
identity. The KB client regression verifies propagation across 18 ordered read wrappers used by
those surfaces. The installed MCP stdio proxy injects the active checkout cwd even when the model
supplies no scope arguments; direct MCP clients can pass the documented `cwd` field or explicit
project/workspace overrides. Legacy conversation windows do not yet carry project identity: current-scope
requests suppress them, while explicit `scope:"all"` preserves the compatibility search.
Legacy `entity_edges` rows likewise have no canonical memory ID to which project visibility can be
attached, so scoped context assembly omits that unscoped graph input and continues to use scoped
`memory_relations`. Migrating or retiring those ID-less edges remains schema debt for E2; they are
still available on explicit unscoped compatibility paths. E2 must also decide how orphaned conflict
rows with a missing side participate in scoped lifecycle alerts; E1 preserves the compatibility
`LEFT JOIN` but excludes a row whose surviving memory cannot establish both scoped sides.

## Adversarial evidence

`unit-test-workspace-memory` inserts one low-confidence active-project fact and then two distractor
buckets that each exceed the tested limit: 12 newer, high-confidence global rows and 12 newer,
high-confidence other-project rows. The local row must still be first for a one-result semantic
query and for one-result list, candidate, recall, briefing, episode, graph, entity, as-of, and
supporting-evidence queries. Explicit all-project scope keeps the local row first and admits the
other-project rows only in the tail. Missing active identity exposes shared/global rows only.

That fixture and the boundary tests caught these ordering defects during implementation:

- SQLite parameter aliases allowed a later `?1` limit to collide with named scope parameters; scope
  parameters now use a reserved high-number range; and
- the semantic path reranked only the caller's requested result count before applying scope, so a
  high-scoring global row could crowd out local evidence. It now reranks the retained candidate
  pool, performs a stable scope-bucket ordering, and only then truncates;
- briefing activity selected the newest episode per session before scope ranking, allowing a newer
  global episode in the same session to hide the active-project episode;
- empty project/workspace strings in the pgvector prefilter matched empty stored values and admitted
  foreign-project rows;
- pgvector's denormalized scope columns could omit a legacy `memory_workspaces` tag (or lag a newer
  canonical tag), so semantic search now resolves memory/unit points to the owning memory and uses
  the same canonical scope SQL before cosine ranking; and
- CLI and session-start requests omitted client cwd, so a correct KB policy still had no active
  project. The marshalling regression now covers search, list, read, and recall, and session briefing
  top-fact/session-scope queries apply the same policy before their limits.

## Verification

Run the focused checks:

```bash
make -C src build/obj/tests/unit-test-workspace-memory
make -C src build/obj/tests/unit-test-kb-client-memory
make -C src build/obj/tests/unit-test-mcp-client-registry
make -C src build/obj/tests/unit-test-cli-v1-delegate
src/build/obj/tests/unit-test-workspace-memory
src/build/obj/tests/unit-test-kb-client-memory
src/build/obj/tests/unit-test-mcp-client-registry
src/build/obj/tests/unit-test-cli-v1-delegate
```

Run the repository gates:

```bash
make -C src all
make -C src check-linking
make -C src lint
make -C src unit-tests
make -C src proposal-links-check
make -C src proposal-reconcile-check
git diff --check
```

The focused regressions and full build pass on the pre-review implementation diff. The pgvector
transport applies the same non-empty scope prefilter before cosine ranking; its linked unit targets
build locally. A real Postgres URL was not present in the local environment, so this record does not
claim a live pgvector query. The link check, lint suite (39 checks), proposal link/reconcile checks,
and full unit suite remain required on the final frozen diff and by PR CI.

This slice does not claim E2 stable project identity/lifecycle, E3 language-aware graph repair, E4
task-conditioned injection, E5 dependency recovery, or E6 promotion results.
