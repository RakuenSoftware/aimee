# MR-01 serving inventory

This inventory tracks the public routes in
`server-go/modules/memory/command_routes.go` and the internal serving views behind
those routes. It complements the unchanged frozen acceptance clauses; it does
not redefine them or certify MR-01. Current serving and authorized historical or
operator inspection have different expected sets.

| Surface | Shared fixture evidence | Remaining work |
|---|---|---|
| `find_facts`, `find_facts_visible`, `find_facts_scoped`, `list`, compatibility `search` | Canonical runtime lifecycle/scope fixture; actual HTTP exact identity sets | Extend the equivalent process fixture to other advertised search views |
| `top_l2_facts`, `load_eval_corpus`, `list_session_scope_priority`, `list_session_scope_priority_like`, `search_facts_patterns_by_keyword` | Same runtime fixture and actual HTTP identity sets | Preserve this coverage on the final candidate |
| `get`, `fact_history` | Runtime current/retained-history population; HTTP exact-ID current/requested-time reads and ten-state fact_history inspection | Complete advertised historical recall equivalence; retain explicit unsupported belief-time/private historical modes |
| `recall` and activation selection | Common HTTP active-context fixture; runtime section backfill; activated generated-card input invalidation | Bring identity, preference, commitment and directive channels into the equivalent common fixture; close release-version gaps |
| `briefing`, `alerts`, `context_block`, `facts`, `assemble_context`, `assemble_typed_context`, `search_assertions`, `ask` | Common HTTP briefing, fact/context blocks, semantic assertion and typed assertion/episode fixtures; dedicated provider-boundary fixtures | Answer evidence, repaired alert inspection and fact/typed source metadata now pass both common audiences on `a4ad77f5d`. Complete remaining channels and metadata checks |
| `ontology`, `query_edges`, `entity_edges`, `search_graph`, `search_graph_as_of`, `entity_profile` | HTTP ontology common fixture; restricted-role derived-parent fixture; hidden-intermediate process test; legacy query_edges scope regression | Entity edges, relation search and profile aggregates now pass the same HTTP population; complete historical process fixtures |
| `scene_list`, `scene_show`, `get_episode`, `episode_cards`, summary views | Restricted-role derived-parent fixture; generated-card edit/revoke/hidden-input cases; common HTTP episode lookup and scene list/membership exact sets | Carry the equivalent fixtures through all advertised process routes |
| `get_provenance`, `link_query`, `list_conflicts` | Restricted-role parent visibility tests; new public audience regressions | Legacy adapters pass common HTTP scope checks; maintain declared inspection semantics |
| Dense vectors, unit/temporal vectors, PageRank and post-fusion filters | Independent runtime lanes with perfect invalid matches; lower-ranked eligible backfill; exact admitted graph nodes/edges | Preserve parent and dependency parity across assembly and release; these internal views are not HTTP methods |
| Directive/reminder matching and briefing | Dedicated normalized expiry, pre-limit exclusion and malformed-time fixtures | Directive matched/fallback recall and session briefing now pass both common and credential process fixtures; remaining reminder inputs and release observations need coverage |
| `validity`, `diagnose_scoped`, `explain_match` | Actual Server/KB decision parity; common runtime/HTTP diagnostic retrieval | Complete diagnostic-purpose and authenticated-context audit; revalidate final candidate |
| Private `get`, `list`, `search`, native recall and exact-version reads | Server process regression matrix; matched current lifecycle decisions plus common-fixture list/search in both actual owners; private version/source checks | Complete common fixture equivalence for every advertised private serving view and release |
| Exports, checkpoints, maintenance, reviews and statistics | Existing authorized inspection and mutation tests | Audit audience/purpose handling independently; ordinary current-serving exclusions must not erase their declared historical/operator behavior |

[Common lane evidence](../../validation/memory-mr01-common-lanes-2026-09-24.md),
[legacy scope process evidence](../../validation/memory-mr01-legacy-scope-2026-09-24.md),
[activation dependency evidence](../../validation/memory-mr01-activation-card-2026-09-24.md),
and [placement decision parity](../../validation/memory-mr01-decision-parity-2026-09-24.md)
identify the actual fixtures and receipts. The
[closeout checklist](memory-reliability-01-closeout.md) remains authoritative for
what is still open, especially final release after concurrent mutation.
