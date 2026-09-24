# MR-01 closeout checklist

MR-01 is the sole active proposal as of 2026-09-24. This checklist follows the
seven clauses in the unchanged frozen acceptance inventory. Passing component
tests do not close an entire clause or authorize advancing to MR-02.

| Gate | Existing implementation/evidence | Remaining closeout work |
|---|---|---|
| A1: one lifecycle/scope fixture across advertised endpoints | Current eligibility predicates; packaged runtime, public command and deployment fixtures; generated-card observation repair passes full memory race and export | Inventory advertised surfaces and run a single equivalent fixture across them; close omitted lanes and diagnostics |
| A2: authorized historical recall excluding erasure/foreign scope | Shared exact-ID read policy, retained fact history and private exact-version inspection | Complete advertised historical semantics and equivalence; explicitly reject unsupported belief-time reconstruction; prove erase/revoke and scope exclusions |
| A3: no graph traversal through hidden intermediates | Eligible graph/PageRank parents and evidence checks, linked-input observations | [Restricted-role two-hop regression](../../validation/memory-mr01-graph-bridge-2026-09-24/race.txt) passes: hidden intermediate blocks both identities, visible evidence restores the path; equivalent process-level fixture remains |
| A4: concurrent pooled scope isolation under non-owner runtime | Transaction-local production settings and restricted-role fixtures | [Two-connection race replay](../../validation/memory-mr01-scope-pool-2026-09-24/race.txt) passes 288 calls from 12 callers, including rollback and cleared session settings; actual process transport coverage remains |
| A5: changes after retrieval invalidate release, preserving earlier decision history | Observed source versions, revalidation on provider attempts, stale-source refusals and durable prepared/admission receipts | Close post-check mutation race and remaining unversioned channels; demonstrate retained decision history at the actual release boundary |
| A6: equal hard gates in lexical, dense-only and graph-only lanes | Independent retrieval lanes use current eligibility; vector/PageRank regressions | Run the common fixture separately through each lane, including backfill and derived dependencies |
| A7: equivalent Server/KB domain decisions; no C fallback | Shared Go owner, C-boundary guard, deployment restart/outage tests | Matched fixtures against both actual processes with recorded decisions and disconnected-module refusal |

The [validity diagnostic implementation](../../validation/memory-validity-2026-09-24.md)
adds a structured decision/reason projection, host-scope checks and CLI/HTTP
forwarding; fresh-process validation remains pending. Required contract work
also remains: host-bound eligibility context across all serving paths,
privileged `include_all` admission audit, and effective or explicitly rejected
legacy parameters. The diagnostic does not substitute for these serving gates.

Closeout requires a traceable result for every gate, current build/export and
boundary checks, process-level placement evidence, and recorded latency and
exclusion behavior. Continue validating the released 0.4.5 installation while
keeping draft application builds in the disposable validation environment.
Do not mark MR-01 complete by deleting requirements, narrowing an advertised
surface, or counting unrelated passing checks.
