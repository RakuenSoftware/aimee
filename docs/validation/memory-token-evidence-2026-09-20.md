# Final-request token evidence binding

MR-03 requires token accounting bound to the final serialized provider request.
Inspection of the Go economizer's OpenAI and Anthropic planners found that
`TokenEvidence` bound provider/model/tokenizer identity and serialized size but
not the content. An equal-length edit could therefore reuse the prior count.

Regressions changed a provider message from `limit=7` to `limit=9` while retaining
previous token evidence. Both planners returned `proof_accepted` for substitutions
in either the baseline or candidate. These local cost proofs do not themselves
authorize dispatch, but their accounting evidence was insufficiently bound.

`TokenEvidence` now requires the SHA-256 digest computed by the counter over the
same serialized body as its count. Both planners compare the digest as well as
size and identity. Missing digests, altered digests, and equal-length substitutions
return `tokenizer_not_local_exact` with an indeterminate cost verdict. Existing
exactly bound fixtures retain their expected verdicts.

Validation: `go test -race ./modules/economizer -count=1` passes (3.322 seconds),
including both baseline/candidate substitution regressions, missing/wrong digest
cases and the existing planner, provenance, proof and admission tests.

This is the existing Go economizer contract used by the MR-03 design. Repository
call-site inspection found the token-evidence constructors in planner tests;
there is no newly enabled production tokenizer or transformation path. The digest
binds a trusted counter's result to bytes; it does not prove that an arbitrary
supplied count is correct. Hard provider token caps, memory source-version binding
and durable preparation/dispatch remain incomplete.
