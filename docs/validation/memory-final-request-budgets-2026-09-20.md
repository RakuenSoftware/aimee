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
and stage grant. Corrected fresh deployment validation is pending. The provider boundary harness adds exact-
fit byte identity, overflow, zero, malformed/token refusal and streaming refusal
checks against the actual host and Go process. The completion endpoint alone is
a fixture; provider captures must remain empty for every refused request.

This is not full MR-03 acceptance. Provider-bound token counters, inherited
operator/task limits, protected optional repacking, canonical source versions,
release evidence and the full transport matrix remain open. The header restricts
each provider request; it does not establish cumulative multi-call task budgets
or durable dispatch receipts.
