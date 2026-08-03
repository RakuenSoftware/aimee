/* bus_wire.h: the event-bus frame encoding.
 *
 * This is the wire contract from docs/proposals/pending/event-bus-wire-spec.md,
 * as decided in docs/dev/EVENT_BUS_DECISIONS.md. It covers framing only: the
 * per-kind payload schema belongs to the event-contract schema, and the region
 * layout belongs to bus_region.
 *
 * Two properties this header is responsible for keeping true:
 *
 *   - The frame is fixed-size and self-describing. BUS_WIRE_HDR_LEN is frozen
 *     by the golden vectors in src/tests/fixtures/bus/, not by this prose, and
 *     the Go reference client is held to the same bytes.
 *
 *   - Nothing here depends on slot_size or inline_budget (D4). Those are
 *     control-region parameters a client reads at attach; a producer uses them
 *     to choose inline-vs-arena placement, but the resulting frame is identical
 *     either way for the same choice. Re-tuning them never re-issues a vector.
 */
#ifndef AIMEE_BUS_WIRE_H
#define AIMEE_BUS_WIRE_H 1

#include <stddef.h>
#include <stdint.h>

/* "BUS0" little-endian. Frame sync; a decode that does not see this stops. */
#define BUS_WIRE_MAGIC 0x30535542u

/* Encoding version, negotiated at attach. Distinct from the region
 * layout_version (D4): this answers "can I decode this frame", not "can I map
 * this region".
 *
 * v2 (arena-payload routing): claims the formerly-reserved trailing 4 bytes as an
 * explicit `generation` field, and defines an ARENA frame's `payload_ref` to be
 * the lease id (not an arena offset — the offset is the lease table's business,
 * retrieved via bus_arena_read_ptr(lease_id, generation)). HDR_LEN is unchanged;
 * a NON-arena v2 frame is byte-identical to v1 (generation is 0, the reserved
 * bytes were already 0), so only arena-frame vectors change.
 *
 * v3 adds BUS_F_MORE for ordered fragments of one correlated request or reply.
 * The fixed layout is unchanged; the host keeps the correlation pending until
 * the final fragment (the first frame without BUS_F_MORE) is delivered. */
#define BUS_WIRE_VERSION 3

/* Frozen by the vectors. Every frame is exactly this many bytes. */
#define BUS_WIRE_HDR_LEN 64

/* An upper bound on a single event's payload, inline or arena. Bounds the
 * capture record size (REC_MAX in D10) and stops a length field from
 * describing a payload no allocator would serve. */
#define BUS_WIRE_MAX_PAYLOAD (1u << 20)

/* hdr_flags. Placement and pattern are separate axes; both are validated. */
#define BUS_F_INLINE       0x0001u /* payload_ref is an in-slot offset */
#define BUS_F_ARENA        0x0002u /* payload_ref is the arena lease id (v2); generation gates it */
#define BUS_F_NOTIFICATION 0x0004u /* one-way; correlation_id is 0 */
#define BUS_F_REQUEST      0x0008u /* correlated; expects a reply */
#define BUS_F_REPLY        0x0010u /* correlated; answers a request */
#define BUS_F_CANCEL       0x0020u /* correlated; cancels an outstanding request */
#define BUS_F_CONTROL      0x0040u /* control-class (D6): never shed, reserved credit */
#define BUS_F_MORE         0x0080u /* more inline request/reply fragments follow */

#define BUS_F_PLACEMENT_MASK (BUS_F_INLINE | BUS_F_ARENA)
#define BUS_F_PATTERN_MASK   (BUS_F_NOTIFICATION | BUS_F_REQUEST | BUS_F_REPLY | BUS_F_CANCEL)
#define BUS_F_KNOWN_MASK \
   (BUS_F_PLACEMENT_MASK | BUS_F_PATTERN_MASK | BUS_F_CONTROL | BUS_F_MORE)

/* Reserved event kinds. Kinds below BUS_KIND_MODULE_BASE are the bus's own;
 * the event-contract schema allocates everything at or above it. Keeping the
 * bus's own traffic in the same frame shape is deliberate: an overflow notice
 * is an ordinary seq-stamped event (D6), not a side channel. */
#define BUS_KIND_ATTACH_REQUEST    1u
#define BUS_KIND_ATTACH_REPLY      2u
#define BUS_KIND_ERROR             3u
#define BUS_KIND_CAPABILITY_ABSENT 4u
#define BUS_KIND_OVERFLOW          5u
#define BUS_KIND_PRODUCER_REAPED   6u
#define BUS_KIND_EPOCH_CHANGE      7u
#define BUS_KIND_CAPABILITY_DENIED 8u
#define BUS_KIND_MODULE_BASE       256u

/* Decoded frame. Field order here follows the wire layout so the two read
 * together; the encoder writes explicit little-endian bytes, so this struct's
 * in-memory layout is never itself the contract. */
typedef struct
{
   uint16_t hdr_flags;
   uint16_t wire_version;
   uint32_t event_kind;
   uint32_t principal_ref; /* attested principal, for the tap and policy */
   uint64_t correlation_id;
   uint64_t seq;         /* host-assigned; 0 until the host stamps it */
   uint64_t logical_ts;  /* ordering across sources without wall-clock trust */
   uint64_t payload_ref; /* INLINE: in-slot offset. ARENA (v2): the lease id. */
   uint32_t payload_len;
   uint32_t src_handle; /* overwritten by the host from the admitted slot */
   uint32_t dst_handle; /* set by the host on routing */
   uint32_t generation; /* v2: ARENA lease generation (0 otherwise) — gates read/release */
} bus_frame_t;

typedef enum
{
   BUS_WIRE_OK = 0,
   BUS_WIRE_ERR_SHORT,       /* fewer than BUS_WIRE_HDR_LEN bytes available */
   BUS_WIRE_ERR_MAGIC,       /* not a frame */
   BUS_WIRE_ERR_VERSION,     /* wire_version this build cannot decode */
   BUS_WIRE_ERR_FLAGS,       /* unknown bit set, or an impossible combination */
   BUS_WIRE_ERR_PAYLOAD_LEN, /* over the bound, or disagrees with placement */
   BUS_WIRE_ERR_CORRELATION, /* correlation_id contradicts the pattern flag */
   BUS_WIRE_ERR_RESERVED,    /* a reserved field was not zero */
   BUS_WIRE_RESULT_COUNT
} bus_wire_result_t;

/* Encode into out. Returns BUS_WIRE_HDR_LEN on success, 0 if outsz is too
 * small or the frame would not survive a decode — the encoder validates the
 * same rules the decoder enforces, so a round trip cannot produce a frame this
 * process would then reject. On failure the output buffer is left untouched;
 * the contract is by return value, not by inspecting the buffer, so a caller
 * must check the return before reading out. */
size_t bus_wire_encode(const bus_frame_t *f, uint8_t *out, size_t outsz);

/* Decode framing only. On BUS_WIRE_OK, *out holds a structurally valid frame;
 * on any error *out is untouched, so a caller cannot act on a partial decode.
 *
 * "Structurally valid" does NOT mean the payload is safe to touch: this
 * function has no geometry, so it cannot bounds-check payload_ref. It exists
 * for callers that genuinely only want framing — the vector tests, offline
 * tools, a capture reader working on materialized bytes. A caller that is
 * going to dereference the payload wants bus_wire_decode_checked instead. */
bus_wire_result_t bus_wire_decode(const uint8_t *in, size_t insz, bus_frame_t *out);

/* Decode and bounds-check in one step. This is the path the host's ingress and
 * every payload-reading consumer must use.
 *
 * Splitting decode from the bounds check made the unsafe call the short,
 * obvious one and left the safe call to documentation, which is a poor way to
 * carry a memory-safety obligation. This entry point makes the safe path the
 * default one: it cannot return BUS_WIRE_OK for a frame whose payload
 * reference lies outside the geometry it was given.
 *
 * Still not sufficient on its own for an arena reference — see
 * bus_wire_check_placement below for what only the lease table can decide. */
bus_wire_result_t bus_wire_decode_checked(const uint8_t *in, size_t insz, uint32_t slot_size,
                                          uint64_t arena_size, bus_frame_t *out);

/* Validate a frame without encoding it. */
bus_wire_result_t bus_wire_validate(const bus_frame_t *f);

/* A decoded payload_ref is NOT an authorization to dereference anything.
 *
 * bus_wire_validate checks that payload_ref is *consistent* with the placement
 * flags — present when there is a payload, absent when there is not. It cannot
 * check that the reference is in bounds, because the bounds are slot_size and
 * arena_size, which live in the control region and are read at attach (D4).
 * A codec that pretended to bounds-check without them would be worse than one
 * that does not: callers would trust a check that never happened.
 *
 * So the contract is explicit. Every consumer of a decoded frame must call
 * bus_wire_check_placement with the live geometry before touching payload
 * bytes, and the host must call it on ingress before routing.
 *
 * It is necessary but NOT sufficient for an arena reference. It answers "does
 * this lie inside the arena", which is all the frame can support. It cannot
 * answer "does this name a live region this reader holds a reference to" —
 * that needs the lease table and its generations, which slice 4 owns (D3). An
 * arena reference of zero is in-bounds here and may still be nonsense; only the
 * lease check can say. A consumer must pass both gates, and slice 4's client
 * API is where the two are made unavoidable rather than merely documented. */
bus_wire_result_t bus_wire_check_placement(const bus_frame_t *f, uint32_t slot_size,
                                           uint64_t arena_size);

/* Stable name for a result, for logs, errors, and the conformance vectors.
 * These strings are part of the cross-language contract: the Go client reports
 * the same names, so a vector's expected result compares as a string. */
const char *bus_wire_result_name(bus_wire_result_t r);

#endif /* AIMEE_BUS_WIRE_H */
