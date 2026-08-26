# Agent-facing code-intelligence E1 validation

- **Slice:** E1. Truthful capability catalog
- **Based on:** E0 merge `352b4682205800ed41714ce7bdd53b4f081f81db` and local-first
  amendment merge `e9626cda560b7e9b1bbf96e48c05c94b62a72c8a`
- **Untreated control:**
  [`agent-facing-code-intelligence-red-baseline.md`](agent-facing-code-intelligence-red-baseline.md)
- **Scope:** discovery, schemas, active-project defaults, installed guidance, and MCP proxy adoption

## Treatment

E1 establishes one public vocabulary for the agent-facing code routes used by generated guidance.
`preview_blast_radius` remains directly advertised in the lean profile while
`index({"command":"preview"})` remains compatible. Its description is discoverable by both
`blast radius` and `preview`; `project` is optional because the MCP proxy supplies `cwd`.

`find_symbol`, callers, and blast preview now default to the project resolved from explicit
`project` or the request cwd. Cross-project symbol and caller lookup requires `scope:"all"`.
Blast preview intentionally rejects all-project scope because its result must describe one indexed
project. Generated Codex, Claude, session-start, attention-guard, and ingress guidance now names
`index({"command":"hybrid"})` for code exploration; `search_graph` remains available for its actual
memory-graph purpose and is not removed as a wire route.

The implementation retains one bounded transitional constraint for a later accepted slice:

- E1 derives the current project from cwd's basename; E2 replaces path-keyed identity with the
  stable workspace project identity and lifecycle contract.

`find_symbol` carries the resolved project through the client and HTTP route into the canonical
index query. The project predicate is applied before the query limit; a one-result regression with
an alphabetically earlier duplicate project proves unrelated namespaces cannot crowd out the active
project. `scope:"all"` retains the compatibility path through the unscoped query.

## Mechanical and installed-surface evidence

The registry contract verifies that:

- the lean profile directly contains `preview_blast_radius`;
- the collapsed and compatibility routes retain matching schemas;
- exact case-insensitive discovery queries `blast radius` and `preview` match the canonical tool;
- only `paths` is required for preview, while `project` and `scope` are admitted;
- `find_symbol` and callers admit `project` plus explicit scope; and
- cwd, explicit project, current/all scope, and invalid scope resolve deterministically.

The client-integration contract rejects `search_graph` in generated code-exploration prompts and
requires `find_symbol`, `ast_grep_search`, and `index`/`hybrid`. The stdio MCP proxy smoke invokes
`preview_blast_radius` with only a path list and proves the canonical name, paths, top-level cwd, and
argument cwd arrive at the server request seam.

Run the E1 checks:

```bash
make -C src build/obj/tests/unit-test-mcp-client-registry
make -C src build/obj/tests/unit-test-client-integrations
make -C src build/obj/tests/unit-test-ingress-preinject
make -C src build/obj/tests/unit-test-cli-mcp-serve
make -C src build/obj/tests/unit-test-kb-client-search
make -C src build/obj/tests/unit-test-kb-http-routes
make -C src build/obj/tests/unit-test-index
src/build/obj/tests/unit-test-mcp-client-registry
src/build/obj/tests/unit-test-client-integrations
src/build/obj/tests/unit-test-ingress-preinject
src/build/obj/tests/unit-test-cli-mcp-serve
src/build/obj/tests/unit-test-kb-client-search
src/build/obj/tests/unit-test-kb-http-routes
src/build/obj/tests/unit-test-index
python3 benchmarks/code-agent-effectiveness/validate_fixtures.py --verify-sources
python3 -m unittest benchmarks.tests.test_agent_code_intelligence_fixtures
make -C src proposal-links-check
make -C src line-check
make -C src lint
make -C src check-linking
make -C src unit-tests
```

This slice does not claim the E2 duplicate-suppression acceptance gate, E3 Python dependency repair,
E4 retrieval lift, E5 resilience, or E6 promotion result.
