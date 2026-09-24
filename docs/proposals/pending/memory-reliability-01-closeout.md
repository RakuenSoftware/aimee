# MR-01 closeout checklist

MR-01 is complete as of 2026-09-24. All seven unchanged frozen acceptance gates
have implementation and validation evidence. The
[final closeout](../../validation/memory-mr01-closeout-2026-09-24.md) records the
exact application, harness, image, process exits and limitations.

| Gate | Completed evidence |
|---|---|
| A1: one lifecycle/scope fixture across advertised endpoints | Final common HTTP fixture: 201 checks; matched private/shared fixture: 58 checks. The [serving inventory](memory-reliability-01-serving-inventory.md) maps ordinary, historical and inspection semantics. |
| A2: authorized historical recall excluding erasure/foreign scope | Final common fixture includes retained historical assertion/graph parents and exact requested-time sets; erased/revoked/foreign records remain excluded. Unsupported belief-time/private modes fail explicitly. |
| A3: no graph traversal through hidden intermediates | [Restricted-role two-hop regression](../../validation/memory-mr01-graph-bridge-2026-09-24/race.txt), equivalent HTTP transitions and final full race replay verify hidden intermediates block traversal and identity disclosure. |
| A4: concurrent pooled scope isolation under non-owner runtime | [Two-connection race replay](../../validation/memory-mr01-scope-pool-2026-09-24/race.txt) and [live HTTP replay](../../validation/memory-mr01-scope-pool-2026-09-24/http/checks.json): 288 calls each, 12 callers, failed-query rollback and cleared settings, under non-owner/non-superuser/NOBYPASSRLS runtime. Final full race replay passes. |
| A5: post-retrieval changes invalidate release; retain earlier history | Versioned roots, parents, auxiliary channels, private active context and hard-rule generations; durable explicit-completion guards. Final native, race and T2/T3 provider-boundary fixtures cover mutation, revocation, outage, crash and recovery with retained receipts. |
| A6: equal hard gates across lexical, dense-only and graph-only lanes | [Independent common lanes](../../validation/memory-mr01-common-lanes-2026-09-24.md), full race replay and final HTTP exact sets verify invalid perfect matches cannot enter or displace eligible backfill; graph nodes, edges and dependencies remain gated. |
| A7: equivalent Server/KB decisions; no C fallback | Final 58 matched process checks, 1,714 T2/T3 deployment checks, all 77 lint gates and native ingress suite pass, including disconnected-owner refusal and recovery. |

The host supplies authenticated purpose/policy/scope and checked source
revocation generations. Legacy arguments are applied or explicitly rejected;
model text cannot widen authority. Validity diagnostics project the serving
decision without inventing evidence or authority. Released CT100 remains healthy
on 0.4.5; the draft application and schema were tested only on CT109.

There is no remaining MR-01 closeout work. MR-02 is next in the sequential program.
