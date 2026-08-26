# P6c-egress transport plan: strict Bedrock HTTPS + CT260 composition

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** complete; roundtable-converged and CT260 validated.
- **Depends on:** merged P6c catalog/authority, P6a SigV4, P6b eventstream,
  P6c IR/stream, and P6c egress engine.
- **Scope:** the kb-internal network transport and an independent CT260 mock gate.
  Public admission, budgets, vault/STS acquisition, and native InvokeModel remain deferred.

## Outcome

Join the already-merged P6 cores across a real TLS socket:

`catalog authority -> request/IR serialization -> SigV4 -> strict HTTPS -> JSON or
eventstream response -> IR`.

The production entry point consumes only a `db2_bedrock_target_t` returned by the
actor/team-bound resolver and trusted temporary credentials. It has no HTTP route and cannot
bypass P2b admission.

## Design invariants

1. **A kb-owned transport seam.** Add a small structured HTTP/1.1 client under `kb/http/` rather
   than importing the server/CLI `agent_bridge` transport. The kb target remains isolated. The API
   accepts bounded name/value headers and byte-counted request bodies; it never accepts a raw
   newline-delimited header string.
2. **Exact signed wire.** The transmitted method, origin-form encoded request target, Host, JSON
   bytes, and signed headers are exactly those produced by
   `kb_bedrock_wire_request_build`. The transport synthesizes only `Content-Length` and
   `Connection: close`; callers cannot provide Host, Content-Length, Transfer-Encoding, or
   Connection twice. Header names/values reject controls, whitespace ambiguity, invalid tokens,
   and duplicate names case-insensitively. Only `POST` plus an origin-form path beginning with
   exactly one `/` is admitted; absolute-form, authority-form, asterisk-form, query, and fragment
   targets are rejected. All length arithmetic is overflow checked and writes loop until complete.
3. **Production endpoint closure.** Production accepts only the partition-correct derived Bedrock
   Runtime host over TLS with hostname verification and the system trust store. The catalog endpoint
   remains empty. There is no C-level endpoint, socket, port, or CA override. On throwaway CT260 only,
   `/etc/hosts` resolves the derived AWS hostname to loopback, the mock binds port 443, and a test CA
   signs a certificate for that exact hostname; Host, DNS name, SNI, SAN verification, port, and wire
   behavior therefore remain production-exact. `SSL_set1_host` and SNI receive the derived hostname,
   and signed Host must equal it. The live harness uses fixed known-answer credentials and never
   accepts production credentials, plaintext, redirects, or arbitrary destinations.
4. **Bounded strict response parser.** Cap the status/header block, header count, header line, body,
   and chunk metadata. Require one valid HTTP/1.1 final response, reject obs-fold, bare LF,
   duplicate/conflicting Content-Length, simultaneous Transfer-Encoding and Content-Length,
   unsupported transfer codings, malformed status/headers/chunks/trailers, premature EOF, surplus
   bytes, informational responses, redirects, responses with neither Content-Length nor chunked
   framing, and duplicate Content-Type. Chunk sizes use checked hexadecimal parsing; extensions and
   nonempty trailers are rejected, and the terminal zero-chunk plus empty trailer terminator is
   mandatory.
5. **Metadata before content.** A headers callback receives status and normalized Content-Type
   before any body callback and chooses `DELIVER`, `DISCARD`, or `ABORT`. Non-2xx selects DISCARD,
   so framing is still consumed and validated without exposing its body to dispatch.
   Bedrock dispatch feeds bytes only for HTTP 200 with exact `application/json` (nonstream) or
   `application/vnd.amazon.eventstream` (stream), allowing optional surrounding OWS and rejecting
   parameters. The transport still consumes and validates the complete bounded framing of non-2xx
   responses before dispatch maps them to provider error; their body is never parsed as IR or logged.
6. **Streaming completion is semantic.** Arbitrary TLS/chunk fragmentation is passed to the merged
   rolling eventstream decoder. Callback abort is distinct from transport error. A syntactically
   complete HTTP response still fails unless `kb_bedrock_stream_finish` proves messageStart,
   messageStop, metadata, complete frames, and exactly one terminal turn stop.
7. **Failure hygiene.** Output/status objects initialize deterministically. Failed requests leave no
   stale body or success status. Mutable request/signing buffers are cleared before release;
   caller-owned credentials are borrowed and never modified, while every transport/dispatch-owned
   copy is cleansed on all exits. AWS
   secret, session token, Authorization, prompt, and completion never enter logs/errors/files.
   The response/callback result is single-assignment: caller abort is terminal, no later callback or
   EOF/finish result may overwrite it, and `kb_bedrock_stream_finish` is never called after abort.
   One monotonic absolute deadline spans bounded asynchronous DNS resolution, connect, TLS handshake,
   complete writes, reads, and best-effort nonblocking shutdown; operations never reset the budget.
   Network calls are SIGPIPE-safe.

## Implementation

- Add `kb/http/kb_http_client.{c,h}` as a new implementation, not layered over or sharing parsing
  with the legacy `kb_tls_client_request*` helpers. It provides a one-shot binary-safe exchange API
  with distinct
  `authority` (always system trust roots and port 443), a headers-gate callback, and:
  - structured request/response metadata types;
  - bounded resolution in a killable, exact-child-reaped helper process plus nonblocking
    connect/TLS I/O against one absolute monotonic deadline, SNI/hostname verification, and
    complete-write helpers;
  - a strict incremental HTTP/1.1 response parser shared by buffered and streaming operation;
  - callback return states distinguishing continue, caller abort, framing failure, and EOF.
- Remove the endpoint-bearing compatibility ABI and its stub entirely. Add an internal production
  dispatch API using `db2_bedrock_target_t`, `kb_bedrock_credentials_t`, and typed buffered/stream
  results; no alternate endpoint-authority path remains.
- Build the full URL/connection tuple from the validated target. The exact signed Host is supplied
  to the transport; the transport independently verifies that it matches the TLS peer authority.
- Add focused transport/dispatch tests using socket/TLS fixtures for short writes, fragmented
  headers/chunks, framing conflicts, media/status rejection, callback abort, response caps, and
  nonstream/stream success.
- Add committed `scripts/run-p6c-egress-ct260.sh` and an independent Python TLS mock. The mock
  verifies canonical request/signature, signed-header set, payload hash, timestamp, optional
  session token, exact encoded path/body, and emits independently encoded JSON/eventstream
  responses fragmented across writes/chunks. It derives SigV4 and eventstream CRCs independently
  and shares no production signer, canonicalizer, decoder, or C fixture. Failure diagnostics report
  only fixed case identifiers and safe lengths, never Authorization, session token, request body,
  content hash, prompt, or completion bytes.

## Validation gates

1. Focused normal and ASAN/UBSAN unit tests for transport + Bedrock dispatch.
2. `make -j$(nproc) server kb`, `make lint`, module-boundary, schema-sync, and kb isolation gates.
3. CT103 real-PG17 P6 catalog/authority gate.
4. CT260 only: sync the branch, build the separately linked test harness, seed a real
   catalog/entitlement actor, enter that actor's tenant scope, call
   `db2_model_bedrock_target_resolve`, and pass that exact owned result into the same dispatch
   function linked by `aimee-kb` for nonstream + stream end to end. The live target links the
   production resolver/dispatch/transport objects without a socket/CA override shim; CT260 supplies
   only its isolated DNS mapping and system test CA. It may not construct a target directly or
   replace those objects with stubs. For every pre-network
   negative, assert the mock's accepted-request counter is unchanged. Negative cases: wrong secret,
   missing/unentitled or
   unsupported target before network, bad CRC, complete-frame semantic truncation, malformed HTTP
   framing, wrong media type, non-2xx, TLS hostname/CA failure, and callback abort.
5. Adversarial roundtable branch review over the complete diff and evidence; repeat until no
   evidence-valid finding survives.

## Explicitly deferred

- P2b public/live egress admission, reservation/settlement, rate limiting, vault signing audit, and
  STS credential acquisition/cache.
- Native InvokeModel families, real AWS account calls, VPC endpoint allowlists, retries, HTTP/2,
  connection pooling, and Bedrock pricing rows.
- Changes to `agent_bridge` unless a branch review proves a shared primitive is unavoidable.
