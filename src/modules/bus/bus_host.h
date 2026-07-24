/* bus_host.h: the bus host — admission, the queue directory, and reaping.
 *
 * The host is the sole admission authority (D1/D2, suite invariant 17). It
 * creates the control and arena regions, and per admitted client a queue-pair
 * region, and hands a client descriptors ONLY for what it may map: the control
 * region read-only, the arena, and its own queue pair. It never hands over
 * another client's queue-pair descriptor, and the queue directory — the map of
 * which slot is whom — is host-private ordinary memory, never a region, so a
 * client cannot enumerate its peers.
 *
 * Admission is gated by an injected decision function (`bus_admit_fn`). Who is
 * admitted — identity, attestation, execution-policy — is owned by
 * module-runtime and is NOT decided here; this slice owns the mechanism (the
 * SOCK_SEQPACKET handshake, slot allocation, SCM_RIGHTS fd grant, heartbeat
 * reaping) and calls the seam. A refused attach is handed a typed reason and no
 * descriptors.
 *
 * This slice does not route events (slice 6) or run flow control (slice 7). It
 * builds the admitted population the router will serve.
 */
#ifndef AIMEE_BUS_HOST_H
#define AIMEE_BUS_HOST_H 1

#include <stdint.h>

#include "bus_arena.h"
#include "bus_region.h"

#define BUS_ATTACH_REQ_MAGIC   0x51524241u /* "ABRQ" */
#define BUS_ATTACH_REPLY_MAGIC 0x50524241u /* "ABRP" */

/* Outcome of an attach attempt, carried in the reply and returned by admission. */
typedef enum
{
   BUS_ATTACH_OK = 0,
   BUS_ATTACH_DENIED_POLICY,  /* admission decision said no */
   BUS_ATTACH_DENIED_VERSION, /* no common wire version */
   BUS_ATTACH_DENIED_NOSLOT,  /* the host is full */
   BUS_ATTACH_PROTOCOL        /* the request was malformed */
} bus_attach_status_t;

/* A client's attach request. principal_class/ref are an opaque identity hint the
 * bus passes to admission unchanged; the bus assigns no meaning to them. */
typedef struct
{
   uint32_t magic;
   uint16_t wire_version_min;
   uint16_t wire_version_max;
   uint32_t principal_class;
   uint32_t principal_ref;
} bus_attach_request_t;

/* The host's reply. On OK it is accompanied by exactly three descriptors
 * (control, arena, this client's queue pair) over SCM_RIGHTS; on any denial it
 * carries no descriptors. */
typedef struct
{
   uint32_t magic;
   uint32_t status; /* bus_attach_status_t */
   uint32_t handle_id;
   uint32_t wire_version;
   uint32_t slot_size;
   uint32_t inline_budget;
   uint32_t queue_capacity;
   uint32_t reserved;
   uint64_t arena_size;
   uint64_t host_epoch;
} bus_attach_reply_t;

/* The admission seam. Returns BUS_ATTACH_OK to admit, or a denial reason. It
 * decides identity/policy only; the host owns slot allocation and fd granting. */
typedef bus_attach_status_t (*bus_admit_fn)(void *ctx, const bus_attach_request_t *req);

/* A directory slot — host-private. A client never sees this or any other slot. */
typedef struct
{
   int in_use;
   uint32_t principal_ref;
   int qpair_fd;
   bus_region_t qpair_region; /* host RW mapping of this client's queue pair */
   bus_qpair_t qpair;         /* host handles into it */
   uint64_t last_heartbeat;   /* last client_heartbeat value the host observed */
   uint64_t heartbeat_at;     /* host clock when it last advanced */
} bus_slot_t;

typedef struct
{
   uint32_t max_slots;
   uint32_t slot_size;
   uint32_t inline_budget;
   uint32_t queue_capacity;
   uint64_t arena_size;
} bus_host_config_t;

typedef struct
{
   bus_region_t control_region; /* host RW mapping */
   int control_fd;
   bus_control_t *control;

   bus_region_t arena_region; /* host RW mapping */
   int arena_fd;
   bus_arena_t arena;

   bus_host_config_t cfg;
   bus_admit_fn admit;
   void *admit_ctx;

   bus_slot_t *slots;
   uint32_t admitted;
} bus_host_t;

typedef enum
{
   BUS_HOST_OK = 0,
   BUS_HOST_ERR_ARG,
   BUS_HOST_ERR_OS,     /* an OS call failed (see errno) */
   BUS_HOST_ERR_REGION, /* a region operation failed */
   BUS_HOST_ERR_REFUSED /* the attach was denied (the reason is in the reply) */
} bus_host_result_t;

/* Create a host: control + arena regions and the arena allocator, an empty
 * directory of cfg.max_slots. admit may be NULL (admits everyone) for tests. */
bus_host_result_t bus_host_create(bus_host_t *h, const bus_host_config_t *cfg, bus_admit_fn admit,
                                  void *admit_ctx);

/* Tear down: unmap and close every region and slot. */
void bus_host_destroy(bus_host_t *h);

/* Process one attach handshake on a connected SOCK_SEQPACKET socket: read the
 * request, consult admission, and on success allocate a slot, create + init its
 * queue-pair region, and send the reply plus the three descriptors; on denial
 * send the typed reply and no descriptors. Returns BUS_HOST_OK when a client was
 * admitted, BUS_HOST_ERR_REFUSED when it was cleanly denied (reply sent), or an
 * error if the handshake itself failed. */
bus_host_result_t bus_host_serve_attach(bus_host_t *h, int conn_fd);

/* Reap slots whose client heartbeat has not advanced within stale_ns. Releases
 * the slot's arena leases (producer and consumer), unmaps and closes its
 * queue-pair region, and frees the slot. `now` and the recorded timestamps share
 * an arbitrary monotonic clock the caller supplies. Returns the number reaped. */
uint32_t bus_host_reap(bus_host_t *h, uint64_t now, uint64_t stale_ns);

/* Bump host_epoch, invalidating every handle and mapping at once (a restart). */
void bus_host_bump_epoch(bus_host_t *h);

uint32_t bus_host_admitted(const bus_host_t *h);

const char *bus_host_result_name(bus_host_result_t r);
const char *bus_attach_status_name(bus_attach_status_t s);

/* ---- fd-passing helpers (shared with the C client, slice 8) ---- */

/* Send `n` (0..3) descriptors plus `len` bytes of `payload` over a SOCK_SEQPACKET
 * socket in one message. Returns 0 or -1 (errno). */
int bus_fd_send(int sock, const void *payload, size_t len, const int *fds, int n);

/* Receive one message: up to `max_fds` descriptors into `fds` (count in *n_out)
 * and up to `len` payload bytes. Returns the payload byte count, or -1 (errno). */
long bus_fd_recv(int sock, void *payload, size_t len, int *fds, int max_fds, int *n_out);

#endif /* AIMEE_BUS_HOST_H */
