/* bus_arena.h: the host-mediated lease allocator over the shared arena region.
 *
 * D3 (docs/dev/EVENT_BUS_DECISIONS.md): a producer requests a lease, fills it,
 * publishes the reference; a consumer reads in place and releases; the host
 * tracks the lease. Payload bytes are never copied through the host — only the
 * lease bookkeeping is host-mediated.
 *
 * The allocator's bookkeeping is therefore HOST-PRIVATE, exactly like the queue
 * directory: the shared arena region holds only payload bytes, and the lease
 * table lives in this process's own memory. A misbehaving client can corrupt
 * arena bytes (the cooperative-arena limit of D2), but it cannot reach the lease
 * table and so cannot forge a lease's lifetime.
 *
 * The lifetime model, verbatim from D3:
 *
 *   bus_arena_alloc            region created, refcount 1, held by the producer
 *   bus_arena_publish to k     +k consumer refs, -1 producer ref (ownership
 *                              transfers to the consumers)
 *   a consumer releases        -1
 *   bus_arena_cancel           -1 (the producer's, on an unpublished lease)
 *   producer reap              drops the producer ref it still holds
 *   consumer reap              drops every ref attributed to that consumer
 *
 * A region returns to the free pool only at refcount zero. Publishing to zero
 * observers takes the refcount straight to zero and reclaims immediately.
 *
 * Two safety rules this interface enforces so a slow or dead peer cannot corrupt
 * a live reader:
 *
 *   - A lease carries a GENERATION, bumped every time its slot is reused. A
 *     consumer read or release validates the generation; a stale one is a typed
 *     error, never a read of whatever now occupies those bytes.
 *   - The per-client live-lease CAP is enforced synchronously at
 *     bus_arena_alloc, before any bytes are written — not at reap, which is
 *     heartbeat-lagging. A client at its cap is refused a new lease immediately.
 *     The cap counts a lease against its allocating slot until the lease is
 *     fully drained (refcount zero), not merely until it is published: this
 *     bounds a producer's total live arena footprint and applies natural
 *     backpressure to a producer whose consumers are not keeping up.
 */
#ifndef AIMEE_BUS_ARENA_H
#define AIMEE_BUS_ARENA_H 1

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* Compile-time ceilings. A slot is a client identity (0..max_slots-1); a lease
 * is an allocation. Both are bounded so the host-private tables are fixed-size
 * and a hostile count cannot ask for an unbounded allocation. */
#define BUS_ARENA_MAX_SLOTS  256
#define BUS_ARENA_MAX_LEASES 4096
#define BUS_ARENA_SLOT_WORDS (BUS_ARENA_MAX_SLOTS / 64)

typedef enum
{
   BUS_ARENA_OK = 0,
   BUS_ARENA_ERR_ARG,       /* null or out-of-range argument */
   BUS_ARENA_ERR_NOSPACE,   /* no free span large enough */
   BUS_ARENA_ERR_NOLEASE,   /* the lease table is full */
   BUS_ARENA_ERR_CAP,       /* the owner is at its per-client live-lease cap */
   BUS_ARENA_ERR_STALE,     /* the generation does not match — the lease was reused */
   BUS_ARENA_ERR_NOTHOLDER, /* the slot does not hold a consumer ref on this lease */
   BUS_ARENA_ERR_STATE      /* the lease is not in a state this call is valid for */
} bus_arena_result_t;

/* A published reference: what the producer puts in a frame's payload_ref/len,
 * plus the generation a consumer must present to read it. */
typedef struct
{
   uint64_t offset; /* into the arena's usable bytes */
   uint32_t len;
   uint32_t generation;
} bus_arena_ref_t;

/* One lease. Host-private; never mapped by a client. */
typedef struct
{
   uint8_t active;
   uint8_t producer_ref;                         /* 0 or 1 */
   uint8_t owner_slot;                           /* the producer that allocated it */
   uint32_t generation;                          /* bumped on each reuse of this table slot */
   uint64_t offset;                              /* span start in the arena */
   uint32_t len;                                 /* span length */
   uint64_t consumer_bits[BUS_ARENA_SLOT_WORDS]; /* which slots hold a consumer ref */
} bus_lease_t;

/* A free span. Host-private free list, kept sorted and coalesced so churn does
 * not strand the arena. */
typedef struct
{
   uint64_t offset;
   uint64_t len;
} bus_free_span_t;

typedef struct
{
   uint8_t *base; /* arena usable bytes (already past the region header) */
   uint64_t size;
   uint32_t max_slots;
   uint32_t per_client_cap;

   bus_lease_t leases[BUS_ARENA_MAX_LEASES];
   uint32_t lease_count; /* high-water of table slots ever used */

   bus_free_span_t freelist[BUS_ARENA_MAX_LEASES + 1];
   uint32_t free_count;

   uint32_t live_per_slot[BUS_ARENA_MAX_SLOTS];
   uint64_t bytes_in_use;

   /* The lease table is host-private and small, but it is NOT single-threaded:
    * co-located producers (D7) allocate and fill from their own threads while the
    * host's pump thread publishes, releases, and reaps. This in-process mutex
    * guards every table transition. It never covers a payload-byte copy — a
    * producer fills, and a consumer reads, the leased span OUTSIDE the lock, safe
    * because a live ref keeps the span from being reclaimed. Contention is
    * negligible: arena is the rare oversized-payload path and each critical
    * section is bookkeeping only. */
   pthread_mutex_t lock;
} bus_arena_t;

/* Initialise over an arena region's usable bytes (from bus_arena_region_attach).
 * per_client_cap bounds live leases per slot; max_slots bounds valid slot ids. */
bus_arena_result_t bus_arena_init(bus_arena_t *a, uint8_t *base, uint64_t size, uint32_t max_slots,
                                  uint32_t per_client_cap);

/* Release the resources bus_arena_init acquired (the table lock). Call once when
 * the host tears the arena down; the arena must be quiescent (no thread inside a
 * lease call). Safe on a zeroed-but-never-initialised arena. */
void bus_arena_fini(bus_arena_t *a);

/* Allocate a span for `owner_slot`. On success *lease_id names the lease and the
 * refcount is 1 (the producer's). Refuses synchronously with ERR_CAP if the
 * owner is at its cap, ERR_NOSPACE if no span fits, ERR_NOLEASE if the table is
 * full. */
bus_arena_result_t bus_arena_alloc(bus_arena_t *a, uint32_t owner_slot, uint32_t len,
                                   uint32_t *lease_id);

/* Writable pointer into the leased span, for the producer to fill before
 * publishing. Valid only while the producer still holds its ref. */
bus_arena_result_t bus_arena_fill_ptr(bus_arena_t *a, uint32_t lease_id, uint8_t **ptr);

/* The reference to publish in a frame. */
bus_arena_result_t bus_arena_ref(const bus_arena_t *a, uint32_t lease_id, bus_arena_ref_t *ref);

/* Publish to `k` observer slots (each 0..max_slots-1). Adds k consumer refs and
 * drops the producer ref. k == 0 reclaims the span immediately (nobody can read
 * it). A slot may not appear twice. */
bus_arena_result_t bus_arena_publish(bus_arena_t *a, uint32_t lease_id,
                                     const uint8_t *observer_slots, uint32_t k);

/* A consumer's read pointer, gated on the generation and on the slot actually
 * holding a ref. ERR_STALE if the lease was reused; ERR_NOTHOLDER if the slot
 * does not hold a ref. */
bus_arena_result_t bus_arena_read_ptr(const bus_arena_t *a, uint32_t lease_id, uint32_t generation,
                                      uint32_t consumer_slot, const uint8_t **ptr);

/* Read pointer + length of a still-producer-held lease's span, for the HOST's own
 * governance tap (capture/replay) to record the payload before routing publishes
 * the lease. This is the one place the host reads arena bytes it did not write;
 * it is not consumer-ref-gated because there is no consumer yet — the producer
 * has filled the span and relinquished it by sending the frame, and the pump has
 * not published it. ERR_STATE once the lease is published (use bus_arena_read_ptr
 * then). *len is the span length; a caller records min(frame.payload_len, *len). */
bus_arena_result_t bus_arena_producer_bytes(const bus_arena_t *a, uint32_t lease_id,
                                            const uint8_t **ptr, uint32_t *len);

/* A consumer releases its ref. Generation-checked. Reclaims at refcount zero. */
bus_arena_result_t bus_arena_release(bus_arena_t *a, uint32_t lease_id, uint32_t generation,
                                     uint32_t consumer_slot);

/* The producer cancels an allocated-but-unpublished lease (its normal error
 * path). Drops the producer ref; reclaims at zero. */
bus_arena_result_t bus_arena_cancel(bus_arena_t *a, uint32_t lease_id);

/* Reap a producer: drop the producer ref on every lease it still owns
 * unpublished-or-published. Published leases become orphaned-but-live and drain
 * as consumers release; unreferenced ones are reclaimed. */
void bus_arena_reap_producer(bus_arena_t *a, uint32_t slot);

/* Reap a consumer: drop every consumer ref attributed to the slot, across all
 * leases, reclaiming any that reach zero. Without this a dead consumer's refs
 * would be permanently unreleasable and repeated deaths would exhaust the arena. */
void bus_arena_reap_consumer(bus_arena_t *a, uint32_t slot);

/* Observers. */
uint32_t bus_arena_live_leases(const bus_arena_t *a, uint32_t slot);
uint64_t bus_arena_bytes_in_use(const bus_arena_t *a);
uint32_t bus_arena_refcount(const bus_arena_t *a, uint32_t lease_id);

const char *bus_arena_result_name(bus_arena_result_t r);

#endif /* AIMEE_BUS_ARENA_H */
