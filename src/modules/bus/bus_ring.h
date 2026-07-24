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
 * Two processes map the same bytes at different addresses, so the shared header
 * stores no pointers: every reference inside it is an offset.
 *
 * ---------------------------------------------------------------------------
 * The split between bus_ring_t and bus_ring_t handle is the safety property
 * ---------------------------------------------------------------------------
 *
 * bus_ring_shared_t is the header in shared memory. A peer — possibly a
 * misbehaving or compromised one — can write to it at any moment. Validating
 * its geometry and then *reading that geometry again* on the hot path would be
 * worthless: the values could change between the check and the use, and the
 * resulting `slots + (index & mask) * slot_size` would address outside the
 * mapping. It is also a data race in the C sense, so the compiler is entitled
 * to reload, hoist, or speculate those fields.
 *
 * So geometry is validated once, at attach, and *copied* into a process-local
 * bus_ring_t. The hot path uses only the local copy. A peer can scribble on
 * the shared header afterwards and it cannot move where this process writes;
 * the worst it can do is corrupt payload bytes, which is the cooperative-arena
 * limit already stated in D2.
 *
 * Only head and tail are read from shared memory on the hot path, and they are
 * atomic, bounds-masked by the local geometry, and cannot produce an
 * out-of-range address whatever value they hold.
 *
 * Concurrency contract:
 *
 *   - The producer calls bus_ring_produce_begin / _commit. Nobody else may.
 *   - The consumer calls bus_ring_consume_begin / _commit. Nobody else may.
 *   - Both may run at once, on different cores, with no coordination.
 *   - bus_ring_init and bus_ring_attach are initialization-phase operations and
 *     are not concurrent with traffic on the same ring (see bus_ring_attach).
 *
 * Publication is a release store on the producer index and consumption is an
 * acquire load of it, which makes the slot's contents visible to the reader
 * before the index that exposes them. The reverse pair on the consumer index
 * does the same for slot reuse.
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
 * speed under load. Asserted by offset, not by hope: see the static asserts
 * below and the offset checks in the tests. */
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

/* The shared header. Both processes map this, so its layout is a contract:
 * fields may only be added in the reserved space, and any change to the shape
 * is a layout_version change (D4). The static asserts below pin every offset,
 * so an inadvertent field insertion becomes a build error rather than a silent
 * disagreement between two processes that map the same bytes.
 *
 * Geometry fields are read exactly once, at attach, and never again — see the
 * header comment. They are declared atomic so that read is not a data race
 * even when a peer is writing them.
 *
 * head and tail are free-running 64-bit counters, masked to index slots. They
 * are never wrapped back to zero, which is what removes the ABA problem a
 * wrapping index would have. They are not magic: a uint64_t does eventually
 * overflow, and states exactly 2^64 operations apart would alias. At a billion
 * operations per second that is about 585 years of continuous traffic on one
 * ring instance, which is far outside any bus lifetime — the guarantee is a
 * stated bound, not an impossibility. */
typedef struct
{
   _Atomic uint32_t magic; /* release-stored last; publishes the header */
   _Atomic uint32_t slot_size;
   _Atomic uint32_t capacity;
   _Atomic uint32_t mask;
   _Atomic uint32_t slots_off;
   _Atomic uint32_t reserved[3];

   _Alignas(BUS_RING_CACHELINE) _Atomic uint64_t head; /* producer writes */
   _Alignas(BUS_RING_CACHELINE) _Atomic uint64_t tail; /* consumer writes */
   _Alignas(BUS_RING_CACHELINE) uint8_t slots[];
} bus_ring_shared_t;

/* Cross-process layout is load-bearing: two processes with different struct
 * layouts would disagree about where the slots start, and attach would reject
 * the ring. Pinning the offsets makes that a compile-time failure here rather
 * than a runtime mystery there. */
_Static_assert(offsetof(bus_ring_shared_t, magic) == 0, "magic must lead the header");
_Static_assert(offsetof(bus_ring_shared_t, head) == BUS_RING_CACHELINE,
               "head must start its own cache line");
_Static_assert(offsetof(bus_ring_shared_t, tail) == 2 * BUS_RING_CACHELINE,
               "tail must not share a cache line with head");
_Static_assert(offsetof(bus_ring_shared_t, slots) == 3 * BUS_RING_CACHELINE,
               "slots must not share a cache line with tail");

/* The process-local handle. Geometry here has been validated and copied; a peer
 * cannot reach it. This is what every hot-path call takes. */
typedef struct
{
   bus_ring_shared_t *shared; /* head/tail live here, and only head/tail */
   uint8_t *slots;            /* resolved once from the validated slots_off */
   uint32_t slot_size;
   uint32_t capacity;
   uint32_t mask;
} bus_ring_t;

/* Bytes needed for a ring of this geometry, including the header and padding.
 * Returns 0 if the geometry is invalid or would overflow. */
size_t bus_ring_bytes(uint32_t slot_size, uint32_t capacity);

/* Lay a ring out in mem and return a handle to it. The caller owns the memory. */
bus_ring_result_t bus_ring_init(void *mem, size_t memsz, uint32_t slot_size, uint32_t capacity,
                                bus_ring_t *out);

/* Adopt a ring another process laid out. Every field the header claims is
 * checked against the buffer actually mapped, then copied into *out.
 *
 * Attach is an initialization-phase operation: it must not run concurrently
 * with traffic on the same ring. The head/tail sanity check reads the two
 * counters separately, and a producer and consumer advancing between those two
 * loads could make a perfectly healthy ring look inconsistent. Rather than
 * paper over that with a retry loop that could still be unlucky, the lifecycle
 * rule is the contract: the host attaches a client's ring before handing the
 * client its descriptor, so there is no traffic to race with. */
bus_ring_result_t bus_ring_attach(void *mem, size_t memsz, bus_ring_t *out);

/* Producer. _begin returns a writable slot, or NULL when the ring is full; the
 * slot is not visible to the consumer until _commit. Splitting them is what
 * makes the write zero-copy: the producer fills the shared slot in place
 * rather than filling a local buffer and copying it in. */
void *bus_ring_produce_begin(const bus_ring_t *r);
void bus_ring_produce_commit(const bus_ring_t *r);

/* Consumer. _begin returns the oldest unread slot, or NULL when the ring is
 * empty; the slot stays valid until _commit releases it for reuse. */
const void *bus_ring_consume_begin(const bus_ring_t *r);
void bus_ring_consume_commit(const bus_ring_t *r);

/* Observers. Both are snapshots: by the time either returns, the other end may
 * have moved. Useful for metrics and for the credit accounting in slice 7,
 * never for deciding whether a _begin will succeed — call _begin for that. */
uint64_t bus_ring_count(const bus_ring_t *r);
uint32_t bus_ring_capacity(const bus_ring_t *r);

const char *bus_ring_result_name(bus_ring_result_t r);

#endif /* AIMEE_BUS_RING_H */
