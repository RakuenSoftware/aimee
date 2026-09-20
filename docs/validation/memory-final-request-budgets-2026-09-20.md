# Final provider request byte admission

MR-03 now has an opt-in final request byte contract for HTTP generation requests.
`X-Aimee-Context-Limits: {"schema_version":1,"max_request_bytes":8192}` applies
to the complete serialized provider body, including system instructions, tools,
memory, JSON escaping and protocol wrappers. Exact fits are admitted; zero is a
literal zero-byte allowance. This contract refuses an oversized body rather than
removing constraints or silently weakening the requested limit.

The Go economizer owns admission through process stage 8/event 11016. The external
host supplies its final body length, SHA-256, provider wire route and exact limits;
the Go response commits to all of those inputs. Only bounded metadata crosses the
existing C bus. A missing stage, malformed reply or mismatched commitment refuses
dispatch, independently of optional economizer reduction. Requests without a
limit retain the existing fast path without an added bus call or body hash.
Memory remains Go; the bus remains C.

The header is bounded to 1024 bytes and copied with the request context, including
background jobs. Duplicate, empty, oversized and folded headers are refused.
Go rejects duplicate JSON keys, unknown fields, wrong versions, null values and
non-integer or out-of-range byte limits. `max_request_tokens`,
`reserved_response_tokens` and `reserved_tool_tokens` currently return
`token_count_unavailable`: no byte heuristic is represented as exact token proof.

Buffered APIs return 413 `request_budget_exceeded`, 400 `request_budget_invalid`
or `token_count_unavailable`, and 503 `request_budget_unavailable`. Streaming APIs
emit explicit errors after SSE headers are committed. Anthropic streaming no
longer reports a fence refusal as an empty successful message. Primary and
fallback requests use the same fence after their respective serialization.

Local validation passes the Go economizer/module registration race suites and
native wire-fence, request-context and module-client regressions. These cover
literal zero, exact fit, one-byte overflow, uint64 precision, duplicate/escaped
keys, expiration, malformed frames, digest binding, unavailable modules, all
three wire routes, and refusal while optional reduction is off. All changed host
translation units compile with warnings treated as errors. The Go handler-only
microbenchmark takes 2.216–2.229 microseconds, 2072 B and 34 allocations per call
on the local i7-14700K; it excludes host hashing and bus latency and is not a
whole-request P95 claim.

Fresh deployment validation found that the optional economizer process was
packaged but omitted from the Server composition. The exact-fit request safely
failed with `request_budget_unavailable` and zero provider dispatches. The
process is now started by default, independently of the existing reduction-mode
settings; a packaging regression checks the actual generated Server composition
and stage grant. Corrected application and matrix harness `140a546a15` pass
**1,093/1,093** checks on fresh `.253` CT 9498 deployments: **683 T2** and
**410 T3**. Each placement passes 200 provider-boundary checks, including 120
new admission checks. Exact-fit requests preserve all provider bytes; overflow,
zero, malformed limits and unavailable token counts produce explicit refusals
without any provider dispatch. Streaming Chat, Responses and Messages refusals
are exercised. Existing memory, semantic, isolation, review, concurrency,
restart, rollback and outage/recovery checks pass.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-140a546a15.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-140a546a15.json),
[provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-140a546a15.json)
and [all nine image identities](memory-shared-reliability-2026-09-20/image-identities-140a546a15.json)
contain bounded verdict/count/digest evidence. Application image:
`sha256:9194b1dc94a091cc7fcf42ec0f1f202b073bd72e1ee2f775a12bb89287dd38ad`.
PostgreSQL and embedder images match the prior validated run. Raw receipts remain
under `/opt/aimee-memory-proposals-evidence/t2-140a546a15` and
`t3-140a546a15`. All nine test containers are stopped; volumes and evidence remain.
The initial `edc602833c` run is retained as failure evidence, not counted as a
passing deployment. The benchmark below is additional to the 1,093 matrix checks.

The opt-in `--budget-benchmark` mode of
`tests/e2e/memory-provider-boundary-e2e.py` records 32 alternating capped/uncapped
pairs per provider on the fresh T3 application. All 200 correctness checks pass;
every timed request dispatches once, returns the expected result, and preserves
the exact baseline body. [Raw bounded timing samples](memory-shared-reliability-2026-09-20/final-budget-timings-140a546a15.json)
include the harness hash and application revision, with no prompt bodies.

| Provider format | Uncapped median / P95 | Capped median / P95 |
|---|---:|---:|
| OpenAI Chat | 74.14 / 97.11 ms | 78.44 / 99.20 ms |
| Anthropic Messages | 70.97 / 88.23 ms | 77.68 / 101.88 ms |

P95 is the nearest-rank statistic. The loopback completion fixture excludes
external model latency; T2 was running on the same host. These small samples
measure admission overhead, not the matched retrieval performance release gate.
In particular, the Anthropic-format fixture's P95 increase exceeds 10%; it is
not evidence of MR-18 performance acceptance. Go handler execution is only about
2.2 microseconds, so optimizing its parser alone cannot resolve the observed
millisecond overhead. The extra bus call and surrounding scheduling need further
measurement before broad default hard-budget enforcement.

This is not full MR-03 acceptance. Provider-bound token counters, inherited
operator/task limits, protected optional repacking, canonical source versions,
release evidence and the full transport matrix remain open. The header restricts
each provider request; it does not establish cumulative multi-call task budgets
or durable dispatch receipts.
