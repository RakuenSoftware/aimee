# P6b implementation plan — AWS eventstream framing decoder (P6 §2, memory-safety core)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice P6b of P6 (Bedrock). Branch off `testing`. The proposal §2 requires decoding the
Bedrock streaming wire `application/vnd.amazon.eventstream` — "hand-parsed binary framing in
C" that MUST "validate the per-message CRC, bound frame/header allocation (reject oversized),
parse headers strictly, and handle AWS error/exception frames explicitly — no unbounded
reads, no trust of attacker-influenced length fields (memory-safety discipline, fuzz-tested)."
P6b ships exactly that: a PURE, OFFLINE, memory-safety-hardened, fuzz-tested framing decoder.
It pairs with the P6a AWS auth core; both are the tested foundation the deferred Bedrock
driver + IR mapping (P6c) call.

## Verified substrate

- No existing eventstream / vnd.amazon / crc32 code (grep clean). No zlib linked and no
  zlib.h on the system — so the decoder carries a **self-contained table-based CRC32**
  (AWS eventstream uses IEEE 802.3 / ISO-HDLC CRC32; the check value CRC32("123456789") =
  0xCBF43926 is the vector anchor). OpenSSL is linked but has no plain CRC32 EVP.
- `src/modules/aws/` (P6a) exists and is **kb-only** (KB_SRCS; kb-target-isolation +
  module-boundary enforce no aws symbols in aimee-server). P6b lands there too.
- Unit-test pattern: `TEST_TARGETS += $(TESTPREFIX)/unit-test-<name>` in tests/Rules.mk,
  linking only the module .o (no OpenSSL needed here — CRC32 is self-contained). Mirror
  unit-test-aws-auth (P6a) / unit-test-org-telemetry.

## The wire format (AWS eventstream message)

- **Prelude (12 bytes):** total_length (uint32 BE), headers_length (uint32 BE), prelude_crc
  (uint32 BE = CRC32 of the first 8 bytes).
- **Headers (headers_length bytes):** each = name_len (u8) + name (name_len bytes) +
  value_type (u8) + value (type-dependent: 0=bool-true[0B], 1=bool-false[0B], 2=byte[1],
  3=short[2], 4=int[4], 5=long[8], 6=bytes[u16 len + data], 7=string[u16 len + data],
  8=timestamp[8], 9=uuid[16]).
- **Payload:** total_length − headers_length − 16 bytes.
- **Message CRC (4 bytes):** uint32 BE = CRC32 of the first (total_length − 4) bytes.

## Design decisions

1. **`src/modules/aws/aws_eventstream.{c,h}` — a pure, no-I/O, kb-only decoder.** No socket,
   no alloc of attacker-sized buffers into the heap without a hard cap. Inputs → typed
   message struct pointing INTO the caller's buffer (zero-copy for the payload/header
   values; the decoder never takes ownership).
2. **Self-contained CRC32** (`aws_es_crc32(buf, len)`), reflected IEEE poly 0xEDB88320,
   table-based, vector-tested (empty→0, "123456789"→0xCBF43926).
3. **`aws_es_decode(buf, len, &msg, &consumed)` — decode ONE complete message from the
   front.** Returns:
   - `AWS_ES_OK` — a full valid message; `msg` filled, `consumed` = total_length.
   - `AWS_ES_NEED_MORE` — fewer than 12 prelude bytes, or fewer than total_length bytes
     buffered (a partial trailing message on the stream — the caller buffers + retries).
   - `AWS_ES_ERROR` — malformed: bad prelude_crc, bad message_crc, total_length < 16 or >
     `AWS_ES_MAX_MESSAGE` (a hard cap, e.g. 16 MiB — reject oversized), headers_length >
     total_length − 16, a header that overruns headers_length (name_len / value len), a
     value_type out of [0,9]. **Every length is validated against the remaining bounds
     BEFORE any read** — no unbounded read, no trust of a length field.
4. **Explicit AWS exception/error frames.** Parse the `:message-type` header; classify the
   message as `AWS_ES_MSG_EVENT` / `AWS_ES_MSG_EXCEPTION` / `AWS_ES_MSG_ERROR` (and expose
   `:event-type` / `:exception-type` / `:content-type` when present) so the caller surfaces
   an AWS error frame as an error, never mistakes it for content (proposal §2).
5. **Bounded header capture.** Up to `AWS_ES_MAX_HEADERS` (e.g. 32) headers captured into a
   fixed array (name + type + a bounded value view); extras beyond the cap → the message
   still decodes but is flagged (or ERROR — decide: flag, since a well-formed message just
   has more headers than we track; the payload + classification remain valid). Header NAMES
   are length-bounded views into the buffer, never copied unbounded.
6. **Stream-clean (buffered-replay caveat).** `consumed` lets the caller advance past a
   decoded message and re-invoke on the remainder; a partial trailing message returns
   NEED_MORE with consumed=0 (nothing consumed), so no byte is dropped or double-read. The
   decoder is stateless per call (the caller owns the rolling buffer).

## Scope (P6b)

1. `src/modules/aws/aws_eventstream.{c,h}` — the CRC32 + `aws_es_decode` + the message/header
   structs + status enum + message-type classification. Add to KB_SRCS only (kb-only).
2. `src/tests/test_aws_eventstream.c` + `unit-test-aws-eventstream` in tests/Rules.mk. Tests:
   (a) CRC32 vectors (empty→0, "123456789"→0xCBF43926);
   (b) a hand-built valid message (a couple of headers incl. `:message-type`=event + a
       payload) decodes: correct headers, payload ptr+len, consumed==total_length, EVENT;
   (c) NEED_MORE on a truncated prelude (<12 B) and on a truncated body (prelude present,
       fewer than total_length bytes);
   (d) ERROR on: bad prelude_crc; bad message_crc; total_length < 16; total_length >
       AWS_ES_MAX_MESSAGE; headers_length > total_length−16; a header name_len that overruns
       headers_length; a value length (string/bytes) that overruns; a value_type == 10;
   (e) an exception frame (`:message-type`=exception, `:exception-type` set) → classified
       AWS_ES_MSG_EXCEPTION;
   (f) two concatenated messages decode sequentially with correct consumed counts;
   (g) **FUZZ sweep**: start from the valid message bytes; for a deterministic set of
       mutations (flip each byte through several values, truncate at every length, corrupt
       each length field), call aws_es_decode and assert it returns exactly one of
       OK/NEED_MORE/ERROR and NEVER reads out of bounds / crashes / loops — the memory-safety
       property. (Build the test with `-fsanitize=address,undefined` if the harness allows a
       per-target flag; else assert bounded return + document ASAN-fuzz as the CI knob.)

## Explicitly deferred (P6c)

The IR mapping (eventstream message → aimee_ir stream event) — the thin layer over aimee_ir.h
that the Bedrock driver drives; the Bedrock backend/driver dispatch + the live streaming
egress; the Converse/native body serializers (P6a-adjacent). P6b is the pure framing decoder
those consume.

## Gate

- `make -j server` links clean (server + kb); aws module stays **kb-only** —
  `make kb-target-isolation-check` + `make module-boundary-check` green (no aws symbols in
  aimee-server).
- `make lint` green; new files clang-format-19 clean; no .sql touched; `make
  schema-sync-check` unaffected.
- `unit-test-aws-eventstream` builds + PASSES — the CRC-validated decode, the bounds/ERROR
  matrix, and the fuzz sweep (no OOB/crash) are the headline.
- No DB / no real-PG gate (pure offline); run-p1-rls-gate.sh untouched.

## Non-goals (P6b)

No IR mapping, no Bedrock driver/egress, no live stream, no Converse/native body serializer,
no network. Pure, bounded, CRC-validated, fuzz-tested eventstream FRAMING decoder, kb-only,
that the deferred Bedrock streaming path (P6c) will consume.

## v2 refinements (roundtable-converged; correctness + resume/integer-safety contract)

- **Value-type mapping CONFIRMED (do not "fix"):** 0=bool-true (0 B), 1=bool-false (0 B),
  2=byte int8 (1 B), 3=short int16 (2 B), 4=int int32 (4 B), 5=**long int64 (8 B, fixed,
  not a varint)**, 6=**byte-array/blob (u16 len + bytes)**, 7=string (u16 len + bytes),
  8=timestamp int64 (8 B, epoch millis), 9=uuid (16 B). (The review's "type 5 is varint /
  type 6 is long" claim was itself contradicted — the AWS eventstream uses FIXED int64 for
  type 5 and a length-prefixed blob for type 6. Implement exactly this table.)
- **CRC contract (explicit):** `aws_es_crc32` = IEEE 802.3 CRC32, reflected, poly 0xEDB88320,
  init 0xFFFFFFFF, xorout 0xFFFFFFFF (check("123456789")==0xCBF43926, check("")==0). The
  prelude_crc and message_crc fields are stored **big-endian** on the wire; read them
  big-endian and compare. prelude_crc = crc32(first 8 bytes); message_crc = crc32(first
  total_length − 4 bytes) — i.e. it DOES cover the prelude + prelude_crc field. The symbol
  is `aws_es_crc32` (NOT a generic `crc32`) to avoid any future link collision.
- **Integer-safety (mandatory guards, called out):** read total_length + headers_length as
  uint32 then WIDEN to `size_t`/`uint64_t` for all arithmetic. BEFORE computing the payload
  length, check `headers_length + 16 <= total_length` (guard the `total_length −
  headers_length − 16` **underflow** — never subtract first). Check `total_length <= len`
  (buffered) and `total_length <= AWS_ES_MAX_MESSAGE` and `total_length >= 16` FIRST. Every
  header read checks `cursor + field_size <= headers_end` before the read (name_len, the u16
  blob/string length, the fixed value widths). No signed arithmetic on a length.
- **ERROR is fatal — the stream is unrecoverable, not resyncable.** On `AWS_ES_ERROR`
  (bad CRC / overrun / oversize / bad value-type) `consumed` is set to 0 and the CONTRACT is
  that the caller MUST tear down the stream/connection — an eventstream cannot be resynced
  past a corrupt frame (there is no frame delimiter to scan to). Documented in the header so
  P6c does not attempt to "skip and continue." `NEED_MORE` also sets consumed=0 (nothing
  consumed); only `AWS_ES_OK` sets consumed=total_length.
- **Error/exception frame headers:** classify on `:message-type` (event | exception | error)
  and expose, when present, `:event-type`, `:content-type`, `:exception-type`, **and
  `:error-code` / `:error-message`** (the error-frame headers). The message PAYLOAD ptr+len
  is reported for ALL classes (an exception frame carries a serialized error body the caller
  reads) — so P6c can surface the AWS error detail, never treat it as model content.
- **Header cap behavior (locked):** at `AWS_ES_MAX_HEADERS` the message STILL decodes — the
  payload + classification + the first N headers are reported and a `headers_truncated` flag
  is set; excess headers are skipped (still bounds-validated, just not captured). It is NOT
  an ERROR (a well-formed message with many headers is valid).
- **P6c API shape locked now (avoid churn):** the message struct exposes decoded multi-byte
  integer header values in HOST byte order (converted from wire big-endian at decode), string/
  blob values as {ptr,len} views into the caller buffer, and a stable classification enum.

### Gate additions

- Test (h): rolling buffer — a COMPLETE message A concatenated with a PARTIAL message B
  (B's prelude present, body short): the first `aws_es_decode` returns OK with
  consumed==A.total_length; a second call on the remainder returns NEED_MORE (consumed=0),
  and after the rest of B arrives it decodes OK. (The real stream-consumer path.)
- Test (i): an ERROR return sets consumed=0 (the caller cannot mis-resume past it).
- Test (j): a decoded int/long header value is byte-swapped correctly from wire big-endian
  to host order.
