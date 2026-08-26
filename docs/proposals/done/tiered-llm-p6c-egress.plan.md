# P6c-egress plan: Bedrock catalog-to-wire integration (P6 §1–3)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Delivered scope archived 2026-07-26.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

Branch from `testing`. This is the first integration slice that joins every merged Bedrock
core: authoritative P6c catalog resolution -> P6a SigV4 -> HTTP dispatch -> P6b eventstream
framing -> P6c Converse IR response/deltas. It exposes a kb-internal production seam for P2b;
it does **not** add the public `/v1/llm/egress` admission route, budget/rate/vault/STS plumbing,
or accept AWS credentials from an HTTP client.

## Invariants

1. **Catalog authority before signing.** Resolve an enabled, actor-entitled Bedrock row on the
   primary through a new actor-and-team-bound `SECURITY DEFINER` function. The caller supplies the
   resolved team plus catalog `model_id`; the definer proves current actor membership in that
   exact team and that team's model entitlement. API/family/target/partition/regions/endpoint
   come from the row. Missing,
   disabled, unentitled, non-Bedrock, or unsupported rows fail before serialization, signing, or
   network I/O. Runtime retains no direct catalog `SELECT`.
2. **Converse only in this slice.** The integrated cores currently implement Converse and
   ConverseStream. A catalog row with `bedrock_api != converse` fails closed before signing;
   there is no OpenAI catch-all and no pretend native-InvokeModel compatibility.
3. **One authoritative target representation.** The DB resolver fills a bounded, owned C target
   containing the routing tuple plus the P6a `bedrock_target_t` inputs. PostgreSQL arrays are
   decoded with explicit caps (<=64 regions/ARNs, existing per-element schema bounds), with
   truncation/parse failure treated as an error. Add an explicit `aws_invoke_region` catalog
   field: it is the SigV4 credential-scope/API-endpoint region and is distinct from an inference
   profile's destination regions. Update `bedrock_target_t`/policy derivation so profile ARNs use
   the invocation region while underlying FM ARNs carry the destination set; do not infer source
   region from array order. Empty endpoint derives the partition-correct Bedrock Runtime origin.
   Production accepts only that exact derived HTTPS origin (a separately configured exact VPC
   endpoint allowlist is deferred); the CT mock override exists only in a test-compiled binary,
   is loopback-only, and uses non-production test credentials. A catalog endpoint can never
   redirect a production session credential. URL/path construction is bounded and rejects control
   chars, query/fragment/userinfo, dot segments, malformed schemes/hosts, and origins outside the
   derived AWS endpoint.
4. **Exact SigV4 wire.** Build `/model/<catalog-model-id>/converse[-stream]`, serialize the
   request with `bedrock_converse_build`, SHA-256 the exact transmitted bytes, and sign `host`,
   `content-type`, `x-amz-date`, `x-amz-content-sha256`, plus the security token when present.
   Percent-encode the raw path once with the same P6a encoder and transmit that exact canonical
   URI; never sign a raw path then send a differently encoded request target. The timestamp and
   short-lived credentials are passed by the trusted kb caller (P2b/P7/STS will
   supply them); secrets are borrowed, never persisted or logged, and wiped from mutable signing
   temporaries where practical. The generic in-process HTTP request builder is hardened as needed
   so long session-token/auth headers cannot silently truncate and Host includes the non-default
   port that was signed. Add a structured header-list transport API (name/value pairs), rejecting
   CR/LF, duplicate or caller-overridden Host/Content-Length/Transfer-Encoding/Authorization,
   and any size overflow; do not pass SigV4 material through newline-delimited raw headers.
5. **Strict response handling.** Non-stream HTTP 200 must parse as Converse JSON and then IR;
   non-2xx, malformed JSON, or malformed Converse fails. Streaming HTTP 200 feeds arbitrary
   transport chunks into a bounded rolling buffer, repeatedly calls `aws_es_decode`, requires
   `:message-type` event/exception/error semantics and JSON content type, copies the bounded
   event-type to NUL-terminated storage, parses each payload, converts it with
   `bedrock_converse_stream_to_deltas`, and synchronously emits borrowed deltas. Decoder ERROR,
   overflow, malformed headers/payload/events, or trailing partial frame aborts the stream.
   HTTP response framing is strict: bounded headers/chunks/content length, invalid or truncated
   chunk framing is an error, and response headers/status are exposed before body delivery so
   only HTTP 200 plus exact `application/vnd.amazon.eventstream` reaches the decoder. A clean HTTP
   EOF after complete frames is still failure unless Converse `messageStop` was observed
   (complete-frame-boundary semantic truncation must not pass).
   No prompt/completion content is persisted or logged.
6. **Reusable seam, no admission bypass.** The new `kb_bedrock_egress` entry point is internal
   code, not an HTTP route. It accepts already-resolved request identity context and trusted
   temporary AWS credentials; P2b will wrap it only after entitlement, rate, budget, vault/STS,
   and key-use audit gates. Tests may inject deterministic time/credentials, production callers
   may not inject a model endpoint outside the catalog.

## Implementation scope

- DB2 schema/grants + `org_model_catalog.{c,h}`: add the actor+team-bound Bedrock target resolver and
  bounded C representation. Extend the real-PG P6 catalog gate with entitled/disabled/wrong-
  provider/cross-actor cases and prove the runtime still has no direct catalog read.
- `kb/kb_bedrock_egress.{c,h}` (or an equivalently isolated kb module): request build, endpoint
  and host/path derivation, SigV4 headers, buffered Converse dispatch/parse, streaming rolling
  decoder and delta callback. Dual-compile only the already-pure Bedrock serializer and stream
  parser dependencies into the kb target, following the vault-core target-isolation pattern;
  do not pull server routing/HTTP code into kb.
- `posix/agent_bridge.c` + public header only if required: make the existing HTTP transport able
  to send the complete structured signed header set without truncation, preserve the exact signed
  Host (including a non-default port), provide bounded strict response metadata/framing, and
  distinguish a callback abort/truncated stream from a clean complete response. Keep existing
  callers source-compatible and add focused transport tests.
- Unit tests: actor+team target resolution shape/caps; endpoint/path/partition/invocation-region
  mapping; exact mock signature inputs; non-stream success and all fail-closed cases; streaming
  frame fragmentation/coalescing, event/exception/error conversion, CRC error, oversized/partial/
  trailing frames, complete-frame semantic truncation, strict HTTP framing/media type, callback
  abort, bounded IR request/body size, and secret-non-disclosure assertions. ASAN/UBSAN the rolling
  decoder integration.
- CT260 integration: run a local mock Bedrock Runtime server that independently verifies the
  SigV4 Authorization signature, signed headers, payload hash, exact received body/path,
  timestamp, and optional
  session token, then returns both a Converse JSON response and a deliberately fragmented AWS
  eventstream response. Seed a real Postgres catalog+entitlement identity, run the production C
  seam end-to-end for non-stream and stream, and include negative wrong-secret, unentitled,
  unsupported-api, bad-CRC, semantically truncated complete-frame stream, malformed HTTP framing,
  and non-2xx cases. The verifier is an independent Python implementation with fixed known-answer
  vectors; it does not call or copy the production C signer/canonicalizer. The mock and runner are
  scripts committed under
  `scripts/`; CT260 remains the target, never the development checkout.

## Gates

- `cd src && rm -f schema_data.h && make -j$(nproc) server`.
- New focused unit/integration targets, `make lint`, module-boundary, kb-target-isolation, and
  schema-sync checks.
- Real PG17 gate on CT103 (including the extended P6 resolver assertions).
- CT260 real-network mock endpoint gate, plus independent ASAN/UBSAN build of the streaming
  integration path.
- Adversarial branch roundtable over the complete proposal, plan, diff, and test evidence;
  incorporate every evidence-valid finding and repeat until no surviving findings.

## Explicitly deferred

P2b's public egress admission path and reserve/rate/settle lifecycle; vault key-use audit and STS
credential acquisition/cache; native `InvokeModel` family adapters; Bedrock pricing additions;
real AWS account validation. This slice provides the production Bedrock transport seam P2b calls,
and proves it against an independent network peer with real Postgres authority.
