/* bus_ring.h: the single-producer/single-consumer ring the event bus is built on.
 *
 * One writer, one reader, no lock. Every ring in the bus has exactly one of
 * each by construction — a client owns its outbound ring and the host owns its
 * inbound ring (D1) — so the SPSC restriction is not a simplification to be
 * revisited later, it is what the topology already guarantees.
 *
 * The ring lives in memory the caller supplies, because in the real bus that
 * memory is a shared mapping. Nothing here knows or cares whether it is a
 * `memfd`, a `malloc`, or a stack buffer; slice 3 supplies the mapping, and
 * keeping this layer ignorant of it is what lets slice 2 land before the D1
 * layout question is settled.
 *
 * Two processes map the same bytes at different addresses, so the ring stores
 * no pointers: every reference inside it is an offset from the header.
 *
 * Concurrency contract:
 *
 *   - The producer calls bus_ring_produce_begin / _commit. Nobody else may.
 *   - The consumer calls bus_ring_consume_begin / _commit. Nobody else may.
 *   - Both may run at once, on different cores, with no coordination.
 *
 * Publication is a release store on the producer index and consumption is an
 * acquire load of it, which is what makes the slot's contents visible to the
 * reader before the index that exposes them. The reverse pair on the consumer
 * index does the same for slot reuse.
 */
#ifndef AIMEE_BUS_RING_H
#define AIMEE_BUS_RING_H 1

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define BUS_RING_MAGIC 0x474e4952u /* "RING" */

/* Indices sit on separate cache lines. Producer and consumer write different
 * lines, so neither invalidates the other's cache on every operation — without
 * this the two ends contend on one line and the ring runs at a fraction of its
 * speed under load. */
#define BUS_RING_CACHELINE 64

/* A ring must hold at least two slots for full and empty to be distinguishable
 * states rather than the same one. */
#define BUS_RING_MIN_CAPACITY 2

typedef enum
{
   BUS_RING_OK = 0,
   BUS_RING_ERR_MEM,      /* the buffer is too small for the requested geometry */
   BUS_RING_ERR_GEOMETRY, /* capacity not a power of two, or a size out of range */
   BUS_RING_ERR_MAGIC,    /* not a ring */
   BUS_RING_ERR_LAYOUT    /* header self-description disagrees with the buffer */
} bus_ring_result_t;

/* The in-memory header. Both processes map this, so its layout is a contract:
 * fields may only be added in the reserved space, and any change to the shape
 * is a layout_version change (D4).
 *
 * head and tail are free-running 64-bit counters, masked to index slots. They
 * are never wrapped back to zero, which is what removes the ABA problem a
 * wrapping index would have: at a billion operations a second a 64-bit counter
 * takes about six centuries to overflow, so "full" and "empty" are always
 * distinguishable from the pair alone and never alias. */
typedef struct
{
   /* Atomic because it is genuinely accessed concurrently: it is the flag that
    * publishes a fully-built header to a peer that may already be polling for
    * it. A release store here pairs with the acquire load in bus_ring_attach,
    * which is the same job a standalone fence would do — except that a fence is
    * invisible to ThreadSanitizer, so the pairing could not be verified. */
   _Atomic uint32_t magic;
   uint32_t slot_size;
   uint32_t capacity; /* power of two */
   uint32_t mask;     /* capacity - 1, so the hot path masks instead of dividing */
   uint32_t slots_off;
   uint32_t reserved[3];

   _Alignas(BUS_RING_CACHELINE) _Atomic uint64_t head; /* producer writes */
   _Alignas(BUS_RING_CACHELINE) _Atomic uint64_t tail; /* consumer writes */
   _Alignas(BUS_RING_CACHELINE) uint8_t slots[];
} bus_ring_t;

/* Bytes needed for a ring of this geometry, including the header and padding.
 * Returns 0 if the geometry is invalid or would overflow. */
size_t bus_ring_bytes(uint32_t slot_size, uint32_t capacity);

/* Lay a ring out in mem. The caller owns the memory; this only writes the
 * header. */
bus_ring_result_t bus_ring_init(void *mem, size_t memsz, uint32_t slot_size, uint32_t capacity,
                                bus_ring_t **out);

/* Adopt a ring another process laid out. Everything the header claims is
 * checked against the buffer actually mapped, because the header may have been
 * written by a process this one does not trust. */
bus_ring_result_t bus_ring_attach(void *mem, size_t memsz, bus_ring_t **out);

/* Producer. _begin returns a writable slot, or NULL when the ring is full; the
 * slot is not visible to the consumer until _commit. Splitting them is what
 * makes the write zero-copy: the producer fills the shared slot in place
 * rather than filling a local buffer and copying it in. */
void *bus_ring_produce_begin(bus_ring_t *r);
void bus_ring_produce_commit(bus_ring_t *r);

/* Consumer. _begin returns the oldest unread slot, or NULL when the ring is
 * empty; the slot stays valid until _commit releases it for reuse. */
const void *bus_ring_consume_begin(const bus_ring_t *r);
void bus_ring_consume_commit(bus_ring_t *r);

/* Observers. Both are snapshots: by the time either returns, the other end may
 * have moved. Useful for metrics and for the credit accounting in slice 7,
 * never for deciding whether a _begin will succeed — call _begin for that. */
uint64_t bus_ring_count(const bus_ring_t *r);
uint32_t bus_ring_capacity(const bus_ring_t *r);

const char *bus_ring_result_name(bus_ring_result_t r);

#endif /* AIMEE_BUS_RING_H */
