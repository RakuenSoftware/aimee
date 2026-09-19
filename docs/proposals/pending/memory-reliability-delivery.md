# Memory reliability delivery tracker

This records implementation evidence and remaining acceptance work for PR #2983.
It does not turn proposal requirements into passing release gates. The scope is
one Go memory implementation in Server/private and KB/shared placements, with
Go module-side producers/consumers using the existing C bus. The bus stays C.

## Authorized work

| Work | Current evidence | Remaining acceptance |
|---|---|---|
| 1. G0 ownership and documentation | Zero native files/descriptor entries; CGO-disabled module and live probe; both C-bus placements including killed-provider refusal and process restart; native labelled audit/calibration and live benchmark policy moved to Go; orphan fusion fixture replaced with production Go assertions | Server KB store/list/get/delete/supersede, search and context-read preserve complete Go envelopes and owner refusals; fact/window search and personal/shared recall composition are Go-owned; the native mutable-array merge is retired; activation snapshots/receipts, private CRUD/search/stats and personal-recall envelopes retain exact IDs, including error receipts with host HTTP classification. Complete the remaining external-caller ownership review, retire stale ABI/fixtures and record per-owner conformance evidence. Broad native-name findings are not themselves C memory implementations. |
| 2. Retrieval compatibility | Versioned whole-record/unit/temporal semantic recall, opt-in Go PageRank, independent-arm RRF with per-arm deduplication | [Compatibility decisions](memory-reliability-retrieval-compatibility.md) are recorded; extend the frozen corpus to the full adversarial matrix and run paired quality measurements. |
| 3. Evaluation and release evidence | Shared injected Go module; isolated corpus/dataset/QA/support/miss runners; scoped runtime-role and transport regressions; complete owner/evaluator race suite added to required packaged-DB2 CI | Initial frozen 105-case input and manifest-bound corpus baselines/per-case receipts are implemented; live CLI/Server benchmarks share the Go owner and refuse partial results; paired quality/performance and the complete CLI/MCP/HTTP/bus restart/failure matrix and required CI remain. |
| 4. MR-01–18 | Existing foundations below | Finish each proposal's acceptance gates and integration dependencies. None is certified complete by the language migration. |

## Supporting indexed-lookup repair

The repeated `scope_required: no active project` during this migration came from
using a linked checkout under `/tmp` against a remote service that had registered
the main checkout under `/home/virant/dev/aimee`. Sending only the client's `cwd`
gave that service neither a matching root nor access to the worktree's Git metadata.

The POSIX thin client now resolves standard linked-worktree metadata locally and
matches the main checkout against registered index roots for indexed reads and
`kb search`. It retains the real `cwd`, explicit/launcher project choices and
`--scope all`. Actual worktree registrations take precedence; unknown or ambiguous
roots do not invent a project. Missing Git or unavailable registry responses leave
the existing server fallback intact. Both native and served-argspec requests use
the same forwarding step. This changes CLI project discovery, not memory identity
or the C event bus. Windows' subprocess stub and nonstandard Git layouts retain
the previous explicit-project/server-fallback behavior.

Regression evidence includes real Git repositories and linked worktrees, nested
directories, shell characters in paths, root boundaries and ambiguity; HTTP tests
exercise both marshallers, registry failure and explicit/environment/all-scope
precedence. Live `index hybrid` and `index investigate` succeed from the migration
worktree without `--project` against the existing remote service.

## Proposal acceptance ledger

| Proposal | Implemented foundation | Work still required for full acceptance |
|---|---|---|
| MR-01 eligibility | Scoped transactions, placement separation, current-hash filters and Go current-validity predicates before lexical/semantic/unit/graph/window/bundle/activation/briefing limits; shared directive/reminder expiry; normalized assertion world/belief-time intervals and current typed-fact evidence closure | Unified temporal/utility policy on every surface, release recheck and race tests |
| MR-02 mutations | Shared KB admission across same-key/edit/legacy verbs, user/model versions, original-author preservation, identity locking, tombstones and atomic extraction actor/job writes | Linked review proposals, personal versioning, expected-version/idempotency contracts, durable guards, invalidation outbox and replay |
| MR-03 budgets | Go typed-context assembly, final personal/shared recall rebudgeting and existing packing limits | Final serialized provider byte/token caps and protected-projection accounting |
| MR-04 lineage | Fact lineage/review and invalidation paths | Independent-family accounting, full derivative closure and restore-resistant erasure |
| MR-05 sufficiency | Typed context and answer abstention | Requirement/coherence coverage of retained evidence and bounded recovery |
| MR-06 receipts | Owner diagnostic parts and postcommit observations | Durable pre-inference receipts, dispatched/acknowledged stages and crash uncertainty |
| MR-07 exploration | Existing adaptive recall limits | Host-issued task contracts, calibrated starvation recovery and operator ceilings |
| MR-08 health | Go lane counters and PageRank timing | Served-population concentration/entropy/fanout/reentry metrics with exact denominators |
| MR-09 ranking | Eligible semantic lanes, RRF, per-arm deduplication, bounded opt-in PageRank | Aggregate prior bounds, final exposure diversity/type floors and adversarial quality gates |
| MR-10 utility | Existing temporal/lifecycle fields | Deterministic utility horizons, authenticated time anchors and historical-access rules |
| MR-11 embeddings | Pinned identity/current hashes, versioned rebuild/cutover/rollback | Full freshness/temporal-activation acceptance under concurrent rebuild/failure |
| MR-12 views | Go briefing/alerts and visible parent checks | Named view/claim-card contracts and full scoped collection-generation cache identity |
| MR-13 task projections | No release claim | Audience intersection, disposable provenance and promotion gates |
| MR-14 hygiene | Existing lint/review surfaces | Proposal-only privileged boundary and rejected-proposal deduplication |
| MR-15 outcomes | Existing feedback/workflow and learning paths | Verified application versus exposure, delayed outcomes and complete task cost |
| MR-16 actions | Existing host authorization boundaries | Exact action evidence reauthorization, idempotent effects and composition budgets |
| MR-17 retries | No release claim | Clean reasoning context with retained real-action journal and replay prevention |
| MR-18 release gates | Go owner tests, live C bus with process restart, isolated evaluators, frozen corpus and manifest-bound per-case baselines | Complete adversarial manifests, temporal reproducibility, paired quality/performance and full surface/restart/failure gates; owner/evaluator PostgreSQL replay is now wired into required CI |

Historical notes are preserved in the [program migration history](memory-reliability-migration-history.md)
and [module migration history](../../modules/memory-migration-history.md). Their
counts and pending statements are dated checkpoints, not current certification.

The full cross-language bus conformance suite passes locally, including memory
termination/restart in both placements. Its egress fixture advertised one
uninitialized extra stage; the fixture now matches the seven shipping stages.
Remote CI exposed a reset-discovery boundary: unconstrained Go-owned embedding
versions were treated as unknown global-dimension tables. Reset discovery now
includes only dimension-bound vector columns; unknown fixed-dimension tables
still refuse reset. Memory replay reports independently after earlier failures.
The unit-semantic fixture now accounts for the existing dimension-specific floor;
restricted-role replay passes locally at both 384 and 1024 dimensions.
The shared-database bootstrap fixture now writes through the real Go memory
handler so canonical KB mutations get their scoped transaction and Server writes
use user scope. Both bootstrap orders and concurrent bootstrap pass with the
existing rows preserved.
