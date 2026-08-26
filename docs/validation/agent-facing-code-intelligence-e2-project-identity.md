# Agent-facing code-intelligence E2 validation

- **Slice:** E2. Current-project identity and lifecycle
- **Based on:** E1-memory merge `ea612958`
- **Policy authority:** proposal merge `5cdb681` and local-first amendment merge `e9626cd`
- **Untreated control:**
  [`agent-facing-code-intelligence-red-baseline.md`](agent-facing-code-intelligence-red-baseline.md)
- **Environment:** Debian; SQLite DB2 compatibility backend for focused lifecycle tests; PostgreSQL
  migration and exact-row SQL remain CI/integration gates

## Treatment

Workspace identity now resolves in a fixed precedence: matching explicit manifest ID, canonical
forge remote plus repository path, then a persisted generated UUID. Git repositories store the UUID
in their common directory so linked worktrees share one identity; non-git projects use the private
`.aimee/project-id` sidecar. Invalid, truncated, ambiguous, or missing identities fail closed. No
index, CLI, HTTP, native MCP, or agent search boundary invents a project from a checkout basename.

DB2 records checkout paths as aliases of a stable project and tracks current, superseded, and
detached generations. Moving an active checkout updates the alias and current generation root
without creating a duplicate project or advancing that generation. Detaching hides the current
generation; re-adding it advances the generation. Source files, code embeddings, knowledge
documents and assets, file/sketch indexes, curator jobs and artifacts, CSS migration units, and
render snapshots carry that generation. Ordinary readers join the owning current project, so the
same logical path may coexist in retained and current generations without an upsert overwriting
history. Generic graph readers admit a code-projection edge only from the visible generation of a
current project; detach removes that derived cache while retaining its projection ledger, and
publication is atomic and bound to the named project. Every
default code and knowledge search resolves the request's active project before retrieval. Explicit
`scope=all` is required for cross-project results and is refused to scoped credentials. Optional
generation fencing rejects stale callers.

For unscoped owners, `project` remains meaningful with `scope=all`: symbol, code text, callers,
hybrid code, ranked knowledge/document search, and artifact facets retrieve the active bucket first,
then query an active-project-excluding tail before applying the final limit. Result rows carry their
stable owning project. This closes the bounded-global-window failure where merely reordering a
truncated corpus could not recover a crowded-out local result.

Purge and garbage collection expose a read-only manifest before mutation. Each target has a
SHA-256 fingerprint over exact physical rows (PostgreSQL `tableoid`, `ctid`, and `xmin`; complete
row identity/content in the SQLite test backend), and the overall hash also binds operation, stable
project, generation, mode, policy criteria, counts, and target fingerprints. Confirmation runs in a
serializable transaction and refuses a changed hash. The audit principal comes only from verified
request context; audit failure blocks deletion. Project-scoped document assets and vector targets
prevent a shared document path from deleting another project's data.
Forced rebuilds clear only the active generation; retained rows are removed only by an explicit,
manifest-confirmed purge or retention GC. Startup hidden-path sanitation follows the same rule.

## Acceptance evidence

The workspace fixtures prove explicit identity precedence, move/re-add stability, linked-worktree
identity sharing, private sidecar permissions, invalid-manifest failure, undersized-buffer failure,
absence of basename fallback, and preservation of an explicitly configured workspace label beside
the stable project identity. Code-index fixtures prove one project row across moves, no generation
advance for an active-checkout move, detached visibility, generation advancement on re-add, and
current-generation reads.

The lifecycle fixture proves:

- detach retains rows but removes the project from current queries;
- detach and destructive lifecycle actions fail closed if verified WORM audit append fails;
- purge dry-run enumerates exact per-target rows and removes only the confirmed project;
- replacing a row without changing the count invalidates the manifest;
- GC binds retention and UTC cutoff, so changing retention invalidates confirmation;
- forged body principals cannot alter audit attribution;
- an anonymous/auth-off request cannot operate lifecycle controls;
- audit failure leaves all destructive targets intact; and
- identical PDF document paths in two projects remain isolated;
- unowned legacy PDF rows are not visible through a project-scoped read; and
- SQLite upgrades create and backfill lifecycle generations and aliases idempotently;
- SQLite upgrades rebuild generation-keyed derived tables without losing legacy IDs, state, or
  payload, and can be re-applied safely;
- same-path file-index, curator-job, MinHash, LSH, CSS, document, asset, and embedding rows survive
  across generations until confirmed GC; and
- pending, superseded, and detached code projections are absent from generic graph recall while the
  visible current projection remains readable.

Search boundary tests place competing project data behind the same query and verify that omitted
scope returns only the active project, explicit all-project scope widens deliberately, and absent
active context returns `scope_required`. Artifact facets, ranked knowledge search, CLI v1, native
MCP, installed proxy, and KB client paths carry the same stable project.
Adversarial limit-two fixtures place a higher-ranked other-project result ahead of local evidence;
the returned order remains active project then other project for symbols, code search, callers,
ranked documents, and facets. Client tests assert that `project` and `scope=all` survive together on
the wire, and direct agent `search_docs` dispatch uses that scoped client path. A preferred project's
generation fence is still enforced when the same request widens its tail with `scope=all`.
The database regressions additionally prove that the all-project symbol and lexical tails exclude
the preferred project before applying their limit. Hybrid search uses the owning project plus file
path as its fusion identity, so the same path in two projects remains two results and the active
project wins the local-first partition. The direct MCP registry contract independently proves that
an explicit project argument is returned unchanged instead of being replaced by cwd inference.
Lifecycle hardening regressions also prove that audit JSON preserves control bytes, an audit helper
cannot append outside the caller's transaction, overlong stable IDs fail as bad requests, and purge
continues safely while legacy deployments are still acquiring the projection tables. Client
lifecycle requests carry no caller-asserted principal; the service derives attribution solely from
the verified request context.
Artifact facet regressions combine release, kind, and project constraints and verify that monotonic
SQL parameter allocation preserves the selected project's evidence without leaking the competing
project or reusing a prior predicate's binding.

## Verification

Focused checks:

```bash
make -C src build/obj/tests/unit-test-workspace
make -C src build/obj/tests/unit-test-workspace-manifest
make -C src build/obj/tests/unit-test-code-project-lifecycle
make -C src build/obj/tests/unit-test-artifacts
make -C src build/obj/tests/unit-test-cmd-hooks-scope
make -C src build/obj/tests/unit-test-kb-http-routes
make -C src build/obj/tests/unit-test-kb-client-search
make -C src build/obj/tests/unit-test-cli-v1-delegate
make -C src build/obj/tests/unit-test-kb
make -C src build/obj/tests/unit-test-server-compute
make -C src build/obj/tests/unit-test-db
make -C src build/obj/tests/unit-test-kb-doc-pdf
make -C src build/obj/tests/unit-test-code-index-ops
make -C src build/obj/tests/unit-test-code-projection
make -C src build/obj/tests/unit-test-code-vectors
make -C src build/obj/tests/unit-test-pgvec
make -C src build/obj/tests/unit-test-sketch
make -C src build/obj/tests/unit-test-cross-repo-deps
make -C src build/obj/tests/unit-test-css-graph
make -C src build/obj/tests/unit-test-css-insights
make -C src build/obj/tests/unit-test-css-migration
make -C src build/obj/tests/unit-test-css-render
make -C src build/obj/tests/unit-test-curator-queue
make -C src build/obj/tests/unit-test-curator-code-unit
make -C src build/obj/tests/unit-test-curator-serve
make -C src build/obj/tests/unit-test-index
make -C src build/obj/tests/unit-test-mcp-client-registry
src/build/obj/tests/unit-test-workspace
src/build/obj/tests/unit-test-workspace-manifest
src/build/obj/tests/unit-test-code-project-lifecycle
src/build/obj/tests/unit-test-artifacts
src/build/obj/tests/unit-test-cmd-hooks-scope
src/build/obj/tests/unit-test-kb-http-routes
src/build/obj/tests/unit-test-kb-client-search
src/build/obj/tests/unit-test-cli-v1-delegate
src/build/obj/tests/unit-test-kb
src/build/obj/tests/unit-test-server-compute
src/build/obj/tests/unit-test-db
src/build/obj/tests/unit-test-kb-doc-pdf
src/build/obj/tests/unit-test-code-index-ops
src/build/obj/tests/unit-test-code-projection
src/build/obj/tests/unit-test-code-vectors
src/build/obj/tests/unit-test-pgvec
src/build/obj/tests/unit-test-sketch
src/build/obj/tests/unit-test-cross-repo-deps
src/build/obj/tests/unit-test-css-graph
src/build/obj/tests/unit-test-css-insights
src/build/obj/tests/unit-test-css-migration
src/build/obj/tests/unit-test-css-render
src/build/obj/tests/unit-test-curator-queue
src/build/obj/tests/unit-test-curator-code-unit
src/build/obj/tests/unit-test-curator-serve
src/build/obj/tests/unit-test-index
src/build/obj/tests/unit-test-mcp-client-registry
```

Repository gates:

```bash
make -C src all
make -C src check-linking
make -C src lint
make -C src unit-tests
make -C src proposal-links-check
make -C src proposal-reconcile-check
git diff --check
```

All 27 focused regressions and the serial full unit suite pass on the rebased pre-review
implementation diff, as do the shipping build, link, proposal-link, proposal-reconciliation, and
`git diff --check` gates. The full suite additionally found three legacy curator fixtures that did
not seed project generations; they now model current and retained generations explicitly, and the
targeted tests plus the complete rerun pass. All substantive lint checks pass; before the candidate
is committed, `docs-gen-check` correctly reports that the two regenerated API references differ
from `HEAD`. The committed candidate must pass that cleanliness check and PR CI. This record does
not claim E3 language-aware graph repair, E4 task-conditioned injection, E5 dependency recovery, or
E6 promotion results.
