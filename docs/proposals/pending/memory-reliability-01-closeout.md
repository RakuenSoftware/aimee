# MR-01 closeout checklist

MR-01 is the sole active proposal as of 2026-09-24. This checklist follows the
seven clauses in the unchanged frozen acceptance inventory. Passing component
tests do not close an entire clause or authorize advancing to MR-02.

| Gate | Existing implementation/evidence | Remaining closeout work |
|---|---|---|
| A1: one lifecycle/scope fixture across advertised endpoints | Current eligibility predicates; packaged runtime, public command and deployment fixtures; generated-card observation repair passes full memory race and export | Inventory advertised surfaces and run a single equivalent fixture across them; close omitted lanes and diagnostics |
| A2: authorized historical recall excluding erasure/foreign scope | Shared exact-ID read policy, retained fact history and private exact-version inspection | Complete advertised historical semantics and equivalence; explicitly reject unsupported belief-time reconstruction; prove erase/revoke and scope exclusions |
| A3: no graph traversal through hidden intermediates | Eligible graph/PageRank parents and evidence checks, linked-input observations | [Restricted-role two-hop regression](../../validation/memory-mr01-graph-bridge-2026-09-24/race.txt) passes: hidden intermediate blocks both identities, visible evidence restores the path; [Equivalent HTTP fixture](../../validation/memory-validity-2026-09-24.md) now passes all three transitions on `386cfaf4c` |
| A4: concurrent pooled scope isolation under non-owner runtime | Transaction-local production settings and restricted-role fixtures | [Two-connection race replay](../../validation/memory-mr01-scope-pool-2026-09-24/race.txt) passes 288 calls from 12 callers, including rollback and cleared session settings; [Live mixed-project replay](../../validation/memory-mr01-scope-pool-2026-09-24/http/checks.json) now passes 288 requests with 12 callers, including failed-query rollback; live runtime role is verified non-owner, non-superuser and NOBYPASSRLS |
| A5: changes after retrieval invalidate release, preserving earlier decision history | Observed source versions, revalidation on provider attempts, stale-source refusals and durable prepared/admission receipts | Close post-check mutation race and remaining unversioned channels; demonstrate retained decision history at the actual release boundary |
| A6: equal hard gates in lexical, dense-only and graph-only lanes | Independent retrieval lanes use current eligibility; vector/PageRank regressions | [Common canonical fixture](../../validation/memory-mr01-common-lanes-2026-09-24.md) passes independent lexical, dense-only and graph-only runtime lanes; HTTP now adds 40 common-fixture checks, including legacy audiences and recall. Lower-ranked eligible vectors backfill LIMIT 3 despite better invalid matches; graph nodes/edges are checked. Activated generated-card dependency repair is being validated |
| A7: equivalent Server/KB domain decisions; no C fallback | Shared Go owner, C-boundary guard, deployment restart/outage tests | [Ten matched lifecycle fixtures](../../validation/memory-mr01-decision-parity-2026-09-24.md) pass 50 checks against both actual owners on `7eb4cf3b1`; its 1714-check matrix covers disconnected-module refusal. Subsequent candidate changes require revalidation |

The [validity diagnostic implementation](../../validation/memory-validity-2026-09-24.md)
adds a structured decision/reason projection, host-scope checks and CLI/HTTP
forwarding; fresh-process validation passes 1714/1714 checks. The
[verified-credential serving repair](../../validation/memory-mr01-serving-scope-2026-09-24.md)
also passes its targeted HTTP regression. Required contract work
also remains: host-bound eligibility context across all serving paths,
privileged `include_all` admission audit, and effective or explicitly rejected
legacy parameters. The diagnostic does not substitute for these serving gates.

Closeout requires a traceable result for every gate, current build/export and
boundary checks, process-level placement evidence, and recorded latency and
exclusion behavior. Continue validating the released 0.4.5 installation while
keeping draft application builds in the disposable validation environment.
Do not mark MR-01 complete by deleting requirements, narrowing an advertised
surface, or counting unrelated passing checks.
