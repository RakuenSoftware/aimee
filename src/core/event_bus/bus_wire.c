/* bus_wire.c: event-bus frame encode/decode. See bus_wire.h.
 *
 * Byte layout, little-endian throughout, BUS_WIRE_HDR_LEN (64) bytes:
 *
 *   off  size  field
 *    0     4   magic
 *    4     2   hdr_flags
 *    6     2   wire_version
 *    8     4   event_kind
 *   12     4   principal_ref
 *   16     8   correlation_id
 *   24     8   seq
 *   32     8   logical_ts
 *   40     8   payload_ref
 *   48     4   payload_len
 *   52     4   src_handle
 *   56     4   dst_handle
 *   60     4   generation (v2: ARENA lease generation; 0 otherwise. Was reserved.)
 *
 * Eight-byte fields sit on eight-byte offsets so a future in-place reader can
 * load them without fixups; the encoder still writes explicit bytes, because
 * the wire may not depend on this host's alignment or endianness.
 */
#include <string.h>

#include <aimee/core/event_bus/bus_wire.h>

#define OFF_MAGIC          0
#define OFF_HDR_FLAGS      4
#define OFF_WIRE_VERSION   6
#define OFF_EVENT_KIND     8
#define OFF_PRINCIPAL_REF  12
#define OFF_CORRELATION_ID 16
#define OFF_SEQ            24
#define OFF_LOGICAL_TS     32
#define OFF_PAYLOAD_REF    40
#define OFF_PAYLOAD_LEN    48
#define OFF_SRC_HANDLE     52
#define OFF_DST_HANDLE     56
#define OFF_GENERATION     60 /* v2: ARENA lease generation (was reserved-zero in v1) */

static void put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v & 0xff);
   p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v & 0xff);
   p[1] = (uint8_t)((v >> 8) & 0xff);
   p[2] = (uint8_t)((v >> 16) & 0xff);
   p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_u64(uint8_t *p, uint64_t v)
{
   for (int i = 0; i < 8; i++)
      p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
   uint64_t v = 0;
   for (int i = 0; i < 8; i++)
      v |= (uint64_t)p[i] << (8 * i);
   return v;
}

/* Exactly one bit set. */
static int exactly_one(unsigned v)
{
   return v != 0 && (v & (v - 1)) == 0;
}

bus_wire_result_t bus_wire_validate(const bus_frame_t *f)
{
   if (f->wire_version != BUS_WIRE_VERSION)
      return BUS_WIRE_ERR_VERSION;

   if ((f->hdr_flags & ~BUS_F_KNOWN_MASK) != 0)
      return BUS_WIRE_ERR_FLAGS;

   /* Exactly one message pattern. A frame that is both a request and a reply,
    * or neither, has no defined routing. */
   if (!exactly_one((unsigned)(f->hdr_flags & BUS_F_PATTERN_MASK)))
      return BUS_WIRE_ERR_FLAGS;

   /* Placement must agree with whether there is a payload at all: a payload
    * needs exactly one of inline/arena, and an empty payload needs neither.
    * Without this a decoder could be handed a payload_ref it has no rule for. */
   unsigned placement = (unsigned)(f->hdr_flags & BUS_F_PLACEMENT_MASK);
   if (f->payload_len > 0)
   {
      if (!exactly_one(placement))
         return BUS_WIRE_ERR_FLAGS;
      if (f->payload_len > BUS_WIRE_MAX_PAYLOAD)
         return BUS_WIRE_ERR_PAYLOAD_LEN;
   }
   else
   {
      if (placement != 0)
         return BUS_WIRE_ERR_FLAGS;
      if (f->payload_ref != 0)
         return BUS_WIRE_ERR_PAYLOAD_LEN;
   }

   /* Fragmentation is deliberately limited to correlated inline traffic. An
    * arena lease already carries a large payload as one reference, while a
    * notification/cancel has no reply lifecycle in which to retain assembly
    * state. A MORE fragment must carry bytes so a producer cannot create an
    * unbounded stream of empty progress markers. */
   if (f->hdr_flags & BUS_F_MORE)
   {
      unsigned pattern = (unsigned)(f->hdr_flags & BUS_F_PATTERN_MASK);
      if ((pattern != BUS_F_REQUEST && pattern != BUS_F_REPLY) || placement != BUS_F_INLINE ||
          f->payload_len == 0)
         return BUS_WIRE_ERR_FLAGS;
   }

   /* generation is an ARENA-only field (v2): it gates the lease read/release.
    * Any non-arena frame must carry 0 (the bytes were reserved-zero in v1). */
   if (!(f->hdr_flags & BUS_F_ARENA) && f->generation != 0)
      return BUS_WIRE_ERR_FLAGS;

   /* A notification has no reply to correlate; the correlated patterns must
    * name one. Enforcing it here means the host never has to guess whether a
    * zero correlation_id was deliberate. */
   if (f->hdr_flags & BUS_F_NOTIFICATION)
   {
      if (f->correlation_id != 0)
         return BUS_WIRE_ERR_CORRELATION;
   }
   else if (f->correlation_id == 0)
   {
      return BUS_WIRE_ERR_CORRELATION;
   }

   return BUS_WIRE_OK;
}

bus_wire_result_t bus_wire_check_placement(const bus_frame_t *f, uint32_t slot_size,
                                           uint64_t arena_size)
{
   bus_wire_result_t r = bus_wire_validate(f);
   if (r != BUS_WIRE_OK)
      return r;
   if (f->payload_len == 0)
      return BUS_WIRE_OK;

   if (f->hdr_flags & BUS_F_INLINE)
   {
      /* An inline payload lives in the ring slot, after the header. Computed in
       * 64-bit and compared against the limit rather than added to it, so a
       * hostile length cannot wrap the bound it is being checked against. */
      uint64_t end = f->payload_ref + (uint64_t)f->payload_len;
      if (end < f->payload_ref)
         return BUS_WIRE_ERR_PAYLOAD_LEN;
      if (f->payload_ref < BUS_WIRE_HDR_LEN || end > (uint64_t)slot_size)
         return BUS_WIRE_ERR_PAYLOAD_LEN;
   }
   else
   {
      /* ARENA (v2): payload_ref is a lease id, not an offset — the span's offset
       * and its bounds against arena_size are the lease table's business
       * (bus_arena_read_ptr, gated on generation + refcount). Here we can only
       * bound the length so a frame cannot claim more than the whole arena. */
      if ((uint64_t)f->payload_len > arena_size)
         return BUS_WIRE_ERR_PAYLOAD_LEN;
   }
   return BUS_WIRE_OK;
}

size_t bus_wire_encode(const bus_frame_t *f, uint8_t *out, size_t outsz)
{
   if (!f || !out || outsz < BUS_WIRE_HDR_LEN)
      return 0;
   if (bus_wire_validate(f) != BUS_WIRE_OK)
      return 0;

   memset(out, 0, BUS_WIRE_HDR_LEN);
   put_u32(out + OFF_MAGIC, BUS_WIRE_MAGIC);
   put_u16(out + OFF_HDR_FLAGS, f->hdr_flags);
   put_u16(out + OFF_WIRE_VERSION, f->wire_version);
   put_u32(out + OFF_EVENT_KIND, f->event_kind);
   put_u32(out + OFF_PRINCIPAL_REF, f->principal_ref);
   put_u64(out + OFF_CORRELATION_ID, f->correlation_id);
   put_u64(out + OFF_SEQ, f->seq);
   put_u64(out + OFF_LOGICAL_TS, f->logical_ts);
   put_u64(out + OFF_PAYLOAD_REF, f->payload_ref);
   put_u32(out + OFF_PAYLOAD_LEN, f->payload_len);
   put_u32(out + OFF_SRC_HANDLE, f->src_handle);
   put_u32(out + OFF_DST_HANDLE, f->dst_handle);
   put_u32(out + OFF_GENERATION, f->generation); /* v2: 0 for non-arena frames */
   return BUS_WIRE_HDR_LEN;
}

bus_wire_result_t bus_wire_decode(const uint8_t *in, size_t insz, bus_frame_t *out)
{
   if (!in || !out)
      return BUS_WIRE_ERR_SHORT;
   if (insz < BUS_WIRE_HDR_LEN)
      return BUS_WIRE_ERR_SHORT;
   if (get_u32(in + OFF_MAGIC) != BUS_WIRE_MAGIC)
      return BUS_WIRE_ERR_MAGIC;

   bus_frame_t f;
   memset(&f, 0, sizeof f);
   f.hdr_flags = get_u16(in + OFF_HDR_FLAGS);
   f.wire_version = get_u16(in + OFF_WIRE_VERSION);
   f.event_kind = get_u32(in + OFF_EVENT_KIND);
   f.principal_ref = get_u32(in + OFF_PRINCIPAL_REF);
   f.correlation_id = get_u64(in + OFF_CORRELATION_ID);
   f.seq = get_u64(in + OFF_SEQ);
   f.logical_ts = get_u64(in + OFF_LOGICAL_TS);
   f.payload_ref = get_u64(in + OFF_PAYLOAD_REF);
   f.payload_len = get_u32(in + OFF_PAYLOAD_LEN);
   f.src_handle = get_u32(in + OFF_SRC_HANDLE);
   f.dst_handle = get_u32(in + OFF_DST_HANDLE);
   f.generation = get_u32(in + OFF_GENERATION);

   bus_wire_result_t r = bus_wire_validate(&f);
   if (r != BUS_WIRE_OK)
      return r;

   /* Committed only after full validation: a caller that ignores the result
    * still cannot read a half-checked frame. */
   *out = f;
   return BUS_WIRE_OK;
}

bus_wire_result_t bus_wire_decode_checked(const uint8_t *in, size_t insz, uint32_t slot_size,
                                          uint64_t arena_size, bus_frame_t *out)
{
   /* Checked separately from bus_wire_decode's own guard: that call is handed a
    * local, so it never sees the caller's pointer and cannot reject a null one
    * on our behalf. Without this the commit below is a null dereference. */
   if (!out)
      return BUS_WIRE_ERR_SHORT;

   bus_frame_t f;
   bus_wire_result_t r = bus_wire_decode(in, insz, &f);
   if (r != BUS_WIRE_OK)
      return r;

   r = bus_wire_check_placement(&f, slot_size, arena_size);
   if (r != BUS_WIRE_OK)
      return r;

   /* Committed only once both the framing and the geometry hold, so this
    * entry point cannot hand back a frame whose payload is out of bounds. */
   *out = f;
   return BUS_WIRE_OK;
}

const char *bus_wire_result_name(bus_wire_result_t r)
{
   switch (r)
   {
   case BUS_WIRE_OK:
      return "OK";
   case BUS_WIRE_ERR_SHORT:
      return "ERR_SHORT";
   case BUS_WIRE_ERR_MAGIC:
      return "ERR_MAGIC";
   case BUS_WIRE_ERR_VERSION:
      return "ERR_VERSION";
   case BUS_WIRE_ERR_FLAGS:
      return "ERR_FLAGS";
   case BUS_WIRE_ERR_PAYLOAD_LEN:
      return "ERR_PAYLOAD_LEN";
   case BUS_WIRE_ERR_CORRELATION:
      return "ERR_CORRELATION";
   case BUS_WIRE_ERR_RESERVED:
      return "ERR_RESERVED";
   case BUS_WIRE_RESULT_COUNT:
   default:
      return "ERR_UNKNOWN";
   }
}
