# P6c-egress engine slice: pure signed requests and response-to-IR

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

**State:** done, merged delivery unit of P6c-egress.

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
   empty segments, and dot segments. The 256-byte raw cap is compatible with this catalog's
   200-byte model-id cap. AWS explicitly permits model/profile ARNs in this greedy URI
   parameter, so the resource slash is preserved while colon and other non-unreserved bytes
   are encoded. The transmitted path is `aws_uri_encode(raw, 0)` and the signer receives the
   identical raw path; tests prove its canonical URI equals the wire path.
3. Requests own a heap body with a 16 MiB semantic cap, bounded raw/encoded paths, derived
   host, `body_len`, payload hash, and `aws_sigv4_result_t`. JSON uses
   `cJSON_PrintPreallocated` with the documented five-byte scratch margin (allocate cap+5,
   then require measured payload <= cap). Before building, an overflow-safe IR preflight
   bounds aggregate strings, array counts, and nested cJSON node count/depth, so the builder
   and its duplicates cannot allocate without a local ceiling. No unbounded print allocator
   or post-build `strlen` is used. The engine signs and exposes structured header KVs only:
   host, content-type, x-amz-date, x-amz-content-sha256, authorization, and the optional
   signed security token. The payload hash header is part of the exact signed set. No raw
   newline-delimited header string is produced.
4. Credentials are borrowed only during build and validated before the existing signer can
   truncate: exact matching `YYYYMMDDTHHMMSSZ`/`YYYYMMDD`, bounded header-safe access key,
   secret at most 255 bytes, and session token shorter than `AWS_SIGV4_TOKEN_MAX`, with no
   C0/DEL/CRLF bytes. The access key uses a conservative ASCII SigV4 Credential grammar and
   `date` must equal the first eight bytes of `amz_date`. This slice hardens
   `aws_sigv4_sign` to reject controls in normalized header names/values and cleanse
   `k_secret`, derived HMAC keys, and signature scratch on every exit after secret material
   exists. Clear/failure cleanses owned request content and signing buffers. Errors are
   typed and content-free.
5. A preflight rejects every semantic IR value the current Converse builder would silently
   omit, default, or misrepresent: unsupported system/message block kinds; documents;
   URL/invalid images; malformed tool-use input/id/name or tool results; unknown roles;
   invalid tool choice; unsupported metadata/service-tier/thinking; and active cache-control
   fields. Counts, pointers, numeric ranges, strings, cJSON nodes, and depth are bounded.
   Provenance-only `raw`/`frontend`/`mutated` fields need not serialize. `top_k` is not a
   normalized Converse `inferenceConfig` field and the catalog has no
   authoritative family-specific additional-field registry. This slice rejects
   `ir->has_top_k` rather than silently dropping it or guessing a vendor field. A later
   breadth slice may add allowlisted per-family mappings.
6. Non-stream parsing accepts an exact-length body of at most 16 MiB. It rejects wire NUL
   and raw `\u0000`, uses `cJSON_ParseWithLengthOpts`, verifies the end pointer, and permits
   only JSON whitespace (`SP`, `HT`, `CR`, `LF`) after one object. A recursive boundary
   rejects decoded C0/DEL in keys/strings, excessive depth/nodes, and duplicate consumed
   semantic keys. A strict Converse validator requires the message, assistant role, content
   tagged unions, stop reason, and nonnegative integral `<=LONG_MAX` usage before mapping.
   The output must be engine-initialized or a prior successful engine response. Parsing is
   into a temporary; success atomically replaces/frees the old value, while failure frees
   and zeroes it. The caller frees success with `aimee_response_free`.
7. Streaming uses an owned rolling buffer capped at one `AWS_ES_MAX_MESSAGE`, processing
   arbitrarily coalesced input incrementally rather than rejecting a feed chunk larger than
   the cap. It uses an offset and compacts only when required, not a full-frame memmove per
   message. Decoded values are borrowed only until the synchronous callback returns. A busy
   guard rejects reentrant feed/finish/clear calls. First fatal returns its specific result
   and poisons; later feed/finish returns `POISONED`. Incomplete finish returns
   `INCOMPLETE_STREAM` once and poisons. Successful finish enters `FINISHED`; later calls are
   invalid. `feed(NULL,0)` is an active no-op. Clear and successful finish cleanse the full
   allocated response buffer, including already-consumed bytes.
8. Every frame rejects decoder errors, truncated or duplicate semantic headers, wrong
   semantic-header types, missing/invalid message classification, embedded controls/NUL,
   exact `application/json` on event/exception frames, payloads above 1 MiB, raw/decoded NUL
   or controls, duplicate consumed JSON keys, excessive JSON depth/nodes, and malformed
   known events. Header-based error frames instead require exactly one bounded string
   error-code and error-message, no class-conflicting headers, empty payload, and no content
   type. Unknown future event types emit no delta only before terminal state. Exception/error
   frames emit one borrowed error delta and then provider-error poison; callback rejection of
   that error delta takes precedence as `CALLBACK_ABORT`.
9. The engine enforces AWS's documented lifecycle: exactly one `messageStart` before known
   content events; `contentBlockStart` is tool-use only; because AWS omits start for text and
   reasoning, their first delta opens the index and synthesizes the required IR block-start
   before its block-delta; later deltas must match the open
   kind; every block stops exactly once and indexes are not reused; `messageStop` occurs once
   with no open blocks; metadata occurs exactly once after it and is terminal. Missing,
   fractional, negative, or >=64 indexes fail closed. Finish requires both terminal events,
   no open blocks, and no partial bytes. `messageStop` retains its reason internally and
   metadata emits the single terminal TURN_STOP with that reason plus usage.

## API and implementation

- Replace `src/kb/kb_bedrock_egress.[ch]` with:
  - typed `kb_bedrock_result_t` outcomes (`OK`, invalid argument/target, too large,
    signing, malformed response/stream, incomplete stream, provider error, callback abort,
    busy, poisoned, internal/allocation failure);
  - borrowed `kb_bedrock_credentials_t`;
  - initialized, non-copyable owned `kb_bedrock_wire_request_t` with `body_len`; build occurs
    into a temporary and publishes only on success, while the header accessor constructs
    borrowed KVs from current fields so no self-referential pointers are stored;
  - response initialization plus exact-length `kb_bedrock_nonstream_parse`;
  - owned opaque rolling stream init/feed/finish/clear API with a synchronous borrowed
    `aimee_delta_t` callback;
  - the legacy dispatch function as an unconditional fail-closed stub that clears output,
    status, and stop state deterministically. Audit remaining callers to prove failure is
    terminal (no fallback/retry/stale read); remove the public raw full-buffer JSON decoder.
- Harden `src/modules/aws/aws_sigv4.c` secret-scratch cleansing and header control rejection.
- Tighten `src/server/aimee_ir_stream.c` where necessary so known content-block events
  require an explicitly present finite integral index rather than defaulting a missing index
  to 0; require exactly one known delta/start union and integral bounded usage. Tighten
  `src/server/aimee_backend_bedrock.c` allocation checks so a validated response cannot
  return partial success after `strdup`/`cJSON_Duplicate` failure; an adapter failure after
  strict engine validation maps to the internal/allocation result.
- Replace the shallow `src/tests/test_kb_bedrock_dispatch.c` fixture with focused pure-engine
  coverage and update `src/tests/Rules.mk`. Test-only linkage to existing Bedrock IR objects
  is in scope; standalone-kb production linkage remains the next slice.

## Fixed caps

- body and buffered non-stream response: 16 MiB semantic bytes; serializer scratch cap+5;
- one rolling eventstream frame: `AWS_ES_MAX_MESSAGE` (16 MiB);
- event/exception JSON payload: 1 MiB;
- raw path: 256 bytes; encoded path: 769 bytes including NUL; host: 128 bytes;
- semantic event/error type strings: 127 bytes; block indexes: 0–63;
- request messages: 1024; total request blocks: 4096; tools: 256; stops: 64; any one IR
  string: 1 MiB; any traversed JSON: 16,384 nodes and depth 64;
- six structured headers; signer-owned constants remain authoritative for authorization,
  canonical request, signed-header list, and security token.

Compile-time assertions pin compatible limits where substrate constants are consumed. All
size additions/multiplications are overflow-checked.

## Gates

- focused deterministic engine tests: three partitions; streaming/non-streaming paths;
  ID and ARN path encoding including the resource slash, canonical-URI/wire equality;
  exact signed headers, body hash and session
  token; boundary caps; tampered targets; malformed dates/secrets/tokens; `top_k`; and
  cleansing/zero-state; every unsupported/coerced IR case; JSON node/depth/count limits;
- exact-length non-stream positive fixture plus empty, over-cap, embedded-NUL, trailing,
  malformed, and pre-populated-output failure cases;
- eventstream byte-at-every-boundary fragmentation and coalescing; bad CRC/length/header
  cases; duplicate/missing/wrong-type semantic headers; content type/JSON faults; every
  lifecycle violation including absent/fractional/64 indexes and text-without-start; unknown
  event; exception/error; callback precedence/reentrancy; partial EOF; empty feed; explicit
  active/poisoned/finished transitions; and a >16 MiB feed of many small frames;
- extract one heap-backed test-only eventstream fixture builder shared by the existing P6b
  unit and engine unit, so semantic-invalid frames retain correct CRCs without two encoders;
- existing P6a/P6b/P6c-IR/P6c-stream units remain green; ASAN/UBSAN and a deterministic
  fragmentation/adversarial sweep; server/kb builds, lint, module-boundary, and target
  isolation checks;
- policy revalidation uses a derived heap scratch cap large enough for the maximum valid
  profile (one profile plus 64 near-cap underlying ARNs), with a max-profile test;
- adversarial full-branch roundtable convergence before merge to `testing`.

## Explicitly deferred

HTTP status/media/EOF/chunked parsing, sockets/TLS, retry/timeout/cancellation, production
linkage into kb admission, catalog resolution calls, vault/STS credentials, budgets and
attribution, test endpoint overrides, CT260 mock deployment, native InvokeModel family
adapters, and pricing rows.
