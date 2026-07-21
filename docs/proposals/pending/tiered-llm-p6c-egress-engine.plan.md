# P6c-egress engine slice — pure signed requests and response-to-IR

**State:** proposed bounded delivery unit of P6c-egress.

This slice replaces the unsafe fixed-buffer `kb_bedrock_egress` scaffold with a pure,
owned request/response engine. It composes the already-delivered authoritative catalog
target, Converse IR adapter, SigV4 signer, eventstream decoder, and ConverseStream IR
mapper. It performs no socket/TLS I/O, no database resolution, no credential acquisition,
no public admission, and no production cross-target linkage. Structured HTTP transport and
the independent CT260 mock peer remain the next slices.

## Binding decisions

1. The only accepted target is the owned `db2_bedrock_target_t` produced by the P6c
   authority resolver. The engine revalidates its complete Converse tuple and requires
   successful least-privilege `bedrock_session_policy` derivation as defense in depth.
   It never accepts a caller-selected endpoint. The legacy dispatch ABI remains only as an
   immediate fail-closed compatibility stub until the transport slice removes its remaining
   compile-time caller.
2. The request host is derived exactly from partition and invocation region:
   `bedrock-runtime.<region>.amazonaws.com` for `aws`/`aws-us-gov`, and
   `bedrock-runtime.<region>.amazonaws.com.cn` for `aws-cn`, always port 443. The raw path is
   `/model/<model_id>/converse[-stream]`; it rejects controls, query/fragment characters,
   empty segments, and dot segments. The transmitted path is `aws_uri_encode(raw, 0)` and
   the signer receives the identical raw path, so canonical URI and wire path cannot drift.
3. Requests own a heap body capped at 16 MiB plus NUL, a bounded raw/encoded path, derived
   host, payload hash, and `aws_sigv4_result_t`. JSON is printed with
   `cJSON_PrintPreallocated`; the engine never calls an unbounded print allocator. It signs
   and exposes structured header KVs only: host, content-type, x-amz-date,
   x-amz-content-sha256, authorization, and the optional signed security token. No raw
   newline-delimited header string is produced.
4. Credentials are borrowed only during build and validated before the existing signer can
   truncate: exact matching `YYYYMMDDTHHMMSSZ`/`YYYYMMDD`, bounded header-safe access key,
   secret at most 255 bytes, and session token shorter than `AWS_SIGV4_TOKEN_MAX`, with no
   control/CRLF bytes. Clear/failure explicitly cleanses owned request content and signing
   buffers. Errors are typed and content-free.
5. `top_k` is not a normalized Converse `inferenceConfig` field and the catalog has no
   authoritative family-specific additional-field registry. This slice rejects
   `ir->has_top_k` rather than silently dropping it or guessing a vendor field. A later
   breadth slice may add allowlisted per-family mappings.
6. Non-stream response parsing accepts an exact-length body of at most 16 MiB, rejects
   embedded NUL, trailing JSON, non-object/malformed Converse responses, and clears a
   pre-populated output on every failure. On success the caller owns the IR response and
   frees it with `aimee_response_free`.
7. Streaming uses an owned rolling buffer capped at one `AWS_ES_MAX_MESSAGE`, processing
   arbitrarily coalesced input incrementally rather than buffering an entire feed chunk.
   Decoded header values remain borrowed only until the synchronous callback returns.
   Fatal framing, semantic, provider, or callback errors poison the stream; later feed or
   finish calls cannot recover. Clear cleanses buffered response bytes.
8. Every frame rejects decoder errors, truncated or duplicate semantic headers, wrong
   semantic-header types, missing/invalid message classification, embedded controls/NUL,
   wrong exact `application/json` content type, payloads above 1 MiB, non-object or trailing
   JSON, and malformed known events. Syntactically valid unknown future event types emit no
   delta. Exception/error frames emit one borrowed error delta synchronously, then return a
   typed provider error and poison.
9. The engine enforces the stream lifecycle beyond the existing JSON mapper: exactly one
   `messageStart`; each block index (0–63) starts once, receives deltas only while open, and
   stops only while open; `messageStop` occurs once with no open blocks; optional metadata
   occurs at most once and only after messageStop; no semantic event follows terminal
   metadata. Finish requires messageStop, no open blocks, and no partial bytes. Metadata
   retains the converged second TURN_STOP usage delta.

## API and implementation

- Replace `src/kb/kb_bedrock_egress.[ch]` with:
  - typed `kb_bedrock_result_t` outcomes (`OK`, invalid argument/target, too large,
    signing, malformed response/stream, incomplete stream, provider error, callback abort,
    poisoned);
  - borrowed `kb_bedrock_credentials_t`;
  - owned `kb_bedrock_wire_request_t`, build/header-access/clear functions;
  - exact-length `kb_bedrock_nonstream_parse`;
  - owned opaque rolling stream init/feed/finish/clear API with a synchronous borrowed
    `aimee_delta_t` callback;
  - the legacy dispatch function as an unconditional fail-closed stub; remove the public
    raw full-buffer JSON callback decoder.
- Tighten `src/server/aimee_ir_stream.c` where necessary so known content-block events
  require an explicitly present integral index rather than defaulting a missing index to 0.
- Replace the shallow `src/tests/test_kb_bedrock_dispatch.c` fixture with focused pure-engine
  coverage and update `src/tests/Rules.mk`. Test-only linkage to existing Bedrock IR objects
  is in scope; standalone-kb production linkage remains the next slice.

## Fixed caps

- body and buffered non-stream response: 16 MiB plus one NUL;
- one rolling eventstream frame: `AWS_ES_MAX_MESSAGE` (16 MiB);
- event/exception JSON payload: 1 MiB;
- raw path: 256 bytes; encoded path: 768 bytes; host: 128 bytes;
- semantic event/error type strings: 127 bytes; block indexes: 0–63;
- six structured headers; signer-owned constants remain authoritative for authorization,
  canonical request, signed-header list, and security token.

Compile-time assertions pin compatible limits where substrate constants are consumed. All
size additions/multiplications are overflow-checked.

## Gates

- focused deterministic engine tests: three partitions; streaming/non-streaming paths;
  ARN path encoding/canonical-URI equality; exact signed headers, body hash and session
  token; boundary caps; tampered targets; malformed dates/secrets/tokens; `top_k`; and
  cleansing/zero-state;
- exact-length non-stream positive fixture plus empty, over-cap, embedded-NUL, trailing,
  malformed, and pre-populated-output failure cases;
- eventstream byte-at-every-boundary fragmentation and coalescing; bad CRC/length/header
  cases; duplicate/missing/wrong-type semantic headers; content type/JSON faults; every
  lifecycle violation; unknown event; exception/error; callback abort; partial EOF; and
  feed-after-fatal/finish;
- existing P6a/P6b/P6c-IR/P6c-stream units remain green; ASAN/UBSAN and a deterministic
  fragmentation/adversarial sweep; server/kb builds, lint, module-boundary, and target
  isolation checks;
- adversarial full-branch roundtable convergence before merge to `testing`.

## Explicitly deferred

HTTP status/media/EOF/chunked parsing, sockets/TLS, retry/timeout/cancellation, production
linkage into kb admission, catalog resolution calls, vault/STS credentials, budgets and
attribution, test endpoint overrides, CT260 mock deployment, native InvokeModel family
adapters, and pricing rows.
