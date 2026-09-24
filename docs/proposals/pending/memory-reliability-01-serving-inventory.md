# MR-01 serving inventory

This inventory tracks the public routes in
`server-go/modules/memory/command_routes.go` and the internal serving views behind
those routes. It complements the unchanged frozen acceptance clauses; it does
not redefine them. MR-01 is complete; the final evidence below supersedes the
intermediate checkpoints. Current serving and authorized historical or
operator inspection have different expected sets.

| Surface | Shared fixture evidence | Final closeout coverage |
|---|---|---|
| `find_facts`, `find_facts_visible`, `find_facts_scoped`, `list`, compatibility `search` | Canonical runtime lifecycle/scope fixture; actual HTTP exact identity sets | Final common 201-check HTTP fixture and full race suite. |
| `top_l2_facts`, `load_eval_corpus`, `list_session_scope_priority`, `list_session_scope_priority_like`, `search_facts_patterns_by_keyword` | Same runtime fixture and actual HTTP identity sets | Final common HTTP exact-set fixture. |
| `get`, `fact_history` | Runtime current/retained-history population; HTTP exact-ID current/requested-time reads and ten-state fact_history inspection | Final historical exact-set fixture; unsupported modes remain explicit. |
| `recall` and activation selection | Common HTTP active-context fixture; runtime section backfill; activated generated-card input invalidation | Final common fixture, private/shared active-context observations and protected provider release. |
| `briefing`, `alerts`, `context_block`, `facts`, `assemble_context`, `assemble_typed_context`, `search_assertions`, `ask` | Common HTTP briefing, fact/context blocks, semantic assertion and typed assertion/episode fixtures; dedicated provider-boundary fixtures | Final auxiliary/typed source versions, both audiences and provider-boundary fixtures. |
| `ontology`, `query_edges`, `entity_edges`, `search_graph`, `search_graph_as_of`, `entity_profile` | HTTP ontology common fixture; restricted-role derived-parent fixture; hidden-intermediate process test; legacy query_edges scope regression | Final historical graph/assertion fixture and full race replay. |
| `scene_list`, `scene_show`, `get_episode`, `episode_cards`, summary views | Restricted-role derived-parent fixture; generated-card edit/revoke/hidden-input cases; common HTTP episode lookup and scene list/membership exact sets | Final common derived-view fixtures and deployment regressions. |
| `get_provenance`, `link_query`, `list_conflicts` | Restricted-role parent visibility tests; new public audience regressions | Final common audience checks; declared inspection semantics preserved. |
| Dense vectors, unit/temporal vectors, PageRank and post-fusion filters | Independent runtime lanes with perfect invalid matches; lower-ranked eligible backfill; exact admitted graph nodes/edges | Full race replay covers independent lanes, backfill and release dependency versions. |
| Directive/reminder matching and briefing | Dedicated normalized expiry, pre-limit exclusion and malformed-time fixtures | Final versioned directive/reminder and session briefing regressions; explicit send completion. |
| `validity`, `diagnose_scoped`, `explain_match` | Actual Server/KB decision parity; common runtime/HTTP diagnostic retrieval | Final 58 matched decisions, authenticated host context and diagnostic authority tests. |
| Private `get`, `list`, `search`, native recall and exact-version reads | Server process regression matrix; matched current lifecycle decisions plus common-fixture list/search in both actual owners; private version/source checks | Final 58 matched decisions, private active-context regression and full transaction-clock replay. |
| Exports, checkpoints, maintenance, reviews and statistics | Existing authorized inspection and mutation tests | Full race and deployment regressions preserve authorized inspection/mutation semantics. |

[Common lane evidence](../../validation/memory-mr01-common-lanes-2026-09-24.md),
[legacy scope process evidence](../../validation/memory-mr01-legacy-scope-2026-09-24.md),
[activation dependency evidence](../../validation/memory-mr01-activation-card-2026-09-24.md),
and [placement decision parity](../../validation/memory-mr01-decision-parity-2026-09-24.md)
identify the actual fixtures and receipts. The
[closeout checklist](memory-reliability-01-closeout.md) records all seven completed gates. The
[final closeout](../../validation/memory-mr01-closeout-2026-09-24.md) links the
final image, process receipts and explicit-completion release evidence.
