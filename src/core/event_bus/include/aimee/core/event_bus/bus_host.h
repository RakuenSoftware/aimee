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
 * admitted — peer identity, attestation, execution-policy — is owned by
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

#include <aimee/core/event_bus/bus_attach.h>
#include <aimee/core/event_bus/bus_arena.h>
#include <aimee/core/event_bus/bus_region_host.h>
#include <aimee/core/event_bus/bus_wire.h>

/* A directory slot — host-private. A client never sees this or any other slot. */
#define BUS_HOST_MAX_KINDS   256
#define BUS_HOST_MAX_PENDING 1024
#define BUS_HOST_MAX_GRANTS  256

/* Outbound patterns authorized for an externally admitted slot. Replies and
 * cancellation are authorized from the host's pending-correlation table; only
 * fresh notifications and requests need manifest grants. */
#define BUS_GRANT_NOTIFY  0x01u
#define BUS_GRANT_REQUEST 0x02u

typedef struct
{
   uint32_t kind;
   uint8_t patterns;
} bus_slot_grant_t;

/* The governance/audit tap: invoked once per event, after seq stamping and
 * before any routing decision (D6). It is the single full-stream observer; a
 * would_block that the host never accepted is not an event and is not tapped. */
typedef void (*bus_tap_fn)(void *ctx, const bus_frame_t *frame, const uint8_t *payload,
                           uint32_t payload_len);

typedef struct
{
   int in_use;
   uint32_t principal_class;
   uint32_t principal_ref;
   int qpair_fd;
   bus_region_t qpair_region; /* host RW mapping of this client's queue pair */
   bus_qpair_t qpair;         /* host handles into it */
   uint64_t last_heartbeat;   /* last client_heartbeat value the host observed */
   uint64_t heartbeat_at;     /* host clock when it last advanced */
   uint64_t dropped;          /* malformed/undeliverable events, counted not silent */
   int enforce_grants;        /* external slot: fail closed on undeclared output */
   uint32_t grant_count;
   bus_slot_grant_t grants[BUS_HOST_MAX_GRANTS];

   /* Block-policy backpressure: a destination-full event is left at this
    * producer's outbound ring head (uncommitted) and retried next pump. It is
    * seq-stamped and tapped exactly once, so those are remembered across
    * retries, and the observers already delivered to are tracked so a fan-out is
    * not double-delivered. */
   int blocked;
   uint64_t blocked_seq;
   uint64_t blocked_delivered[BUS_ARENA_SLOT_WORDS];

   /* Arena-notification fan-out: the observer set is snapshotted here when the
    * lease is published (once, at first sight) so retries deliver the reference
    * to exactly the set that was given a consumer ref — a subscriber that arrives
    * mid-retry holds no ref and must not receive an unreadable reference. */
   uint64_t arena_targets[BUS_ARENA_SLOT_WORDS];
} bus_slot_t;

/* Per-kind overflow policy (D5). Block is the safe default: a full destination
 * holds the event at the producer's ring head rather than losing it. Shed is
 * opt-in per kind: a full destination is told, via a typed overflow event, that
 * it lost one — never a silent drop. */
typedef enum
{
   BUS_KIND_BLOCK = 0,
   BUS_KIND_SHED
} bus_kind_policy_t;

/* A registered event kind: who observes its notifications, who serves its
 * requests, and its overflow policy. Observers is a slot bitmap; server is a
 * slot index or NONE. */
typedef struct
{
   int in_use;
   uint32_t kind;
   uint64_t observers[BUS_ARENA_SLOT_WORDS];
   int32_t server; /* serving slot, or -1 */
   bus_kind_policy_t policy;
} bus_kind_t;

/* The inline body of an overflow event: which event was shed, and to whom. A
 * consumer can enumerate exactly the seq values it lost from these alone (D6). */
typedef struct
{
   uint64_t shed_seq;
   uint32_t shed_kind;
   uint32_t dst_slot;
} bus_overflow_t;

/* An outstanding request, so a reply can be routed back to its requester.
 *
 * Correlation ids are chosen by each client for itself, so they are unique only
 * within one client: two clients calling the same server will eventually pick
 * the same number. The host therefore keeps both -- the requester's id, and a
 * host-assigned id that is unique across the bus and is the only one the server
 * ever sees. Requests are rewritten to server_correlation_id on the way out and
 * replies back to correlation_id on the way in, so each caller sees its own
 * numbering and the server can key its work on something unambiguous. */
typedef struct
{
   int in_use;
   int request_open;
   uint64_t correlation_id;
   uint64_t server_correlation_id;
   uint32_t requester;
   uint32_t server;
} bus_pending_t;

typedef struct
{
   uint32_t max_slots;
   uint32_t slot_size;
   uint32_t inline_budget;
   uint32_t queue_capacity;
   uint64_t arena_size;
} bus_host_config_t;

typedef struct bus_host
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
   bus_attach_status_t (*attach_hook)(void *ctx, struct bus_host *host, uint32_t slot,
                                      const bus_attach_request_t *request);
   void *attach_hook_ctx;

   bus_slot_t *slots;
   uint32_t admitted;

   bus_kind_t kinds[BUS_HOST_MAX_KINDS];
   bus_pending_t pending[BUS_HOST_MAX_PENDING];
   uint64_t seq;                     /* host-assigned monotonic dispatch order */
   uint64_t next_server_correlation; /* host-unique ids handed to servers */

   bus_tap_fn tap;
   void *tap_ctx;
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
 * directory of cfg.max_slots. The admission callback receives the attach fd so
 * daemon policy can authenticate its OS peer. admit may be NULL for trusted
 * in-process socketpairs and tests; an external listener must provide one. */
bus_host_result_t bus_host_create(bus_host_t *h, const bus_host_config_t *cfg, bus_admit_fn admit,
                                  void *admit_ctx);

/* Install or replace admission policy before accepting external clients. The
 * trusted in-process bootstrap attaches before this seam is installed. */
void bus_host_set_admission(bus_host_t *h, bus_admit_fn admit, void *admit_ctx);

/* Tear down: unmap and close every region and slot. */
void bus_host_destroy(bus_host_t *h);

/* Process one attach handshake on a connected SOCK_SEQPACKET socket: read the
 * request, consult admission, and on success allocate a slot, create + init its
 * queue-pair region, and send the reply plus the three descriptors; on denial
 * send the typed reply and no descriptors. Returns BUS_HOST_OK when a client was
 * admitted, BUS_HOST_ERR_REFUSED when it was cleanly denied (reply sent), or an
 * error if the handshake itself failed. */
bus_host_result_t bus_host_serve_attach(bus_host_t *h, int conn_fd);
bus_host_result_t bus_host_serve_attach_ex(bus_host_t *h, int conn_fd, uint32_t *slot_out);

/* Release one admitted slot immediately. Runtime owners use this only after an
 * OS-backed process handle proves the client has exited; ordinary liveness
 * remains heartbeat-reaped. The caller must hold the same host lock used for
 * attach, pump, and reap. */
bus_host_result_t bus_host_release_slot(bus_host_t *h, uint32_t slot);

/* Install the daemon-owned capability binder. Admission authenticates the
 * request before allocation; this hook runs after a slot exists but before its
 * descriptors are granted, and may subscribe/serve only the kinds authorized
 * for that module identity. A non-OK result rolls the slot back and grants no
 * descriptors. */
void bus_host_set_attach_hook(bus_host_t *h,
                              bus_attach_status_t (*hook)(void *ctx, bus_host_t *host,
                                                          uint32_t slot,
                                                          const bus_attach_request_t *request),
                              void *ctx);

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

/* Set the governance/audit tap. Pass NULL to clear. */
void bus_host_set_tap(bus_host_t *h, bus_tap_fn fn, void *ctx);

/* Register `slot` as an authorized observer of `event_kind` (its notifications).
 * Subscription is the authorization: a client never receives a kind it did not
 * subscribe to. */
bus_host_result_t bus_host_subscribe(bus_host_t *h, uint32_t slot, uint32_t event_kind);

/* Register `slot` as the single server for `event_kind` (its requests). A second
 * server for the same kind is refused. */
bus_host_result_t bus_host_serve_kind(bus_host_t *h, uint32_t slot, uint32_t event_kind);
int bus_host_kind_has_server(const bus_host_t *h, uint32_t event_kind);

/* Mark an admitted slot as manifest-governed, then grant the fresh outbound
 * patterns it declared. A governed slot with no matching grant cannot inject
 * that notification/request kind. Trusted in-process socketpair clients remain
 * unrestricted unless explicitly marked. */
bus_host_result_t bus_host_enforce_grants(bus_host_t *h, uint32_t slot);
bus_host_result_t bus_host_grant_outbound(bus_host_t *h, uint32_t slot, uint32_t event_kind,
                                          uint8_t patterns);

/* Set a kind's overflow policy. Default is BUS_KIND_BLOCK. */
bus_host_result_t bus_host_set_kind_policy(bus_host_t *h, uint32_t event_kind,
                                           bus_kind_policy_t policy);

/* Drain every admitted client's outbound ring once, routing each event: stamp
 * seq, offer it to the tap, then deliver — notifications to the kind's authorized
 * observers, a request to the kind's server (or a synthesized capability_absent
 * reply), a reply point-to-point to its requester, a cancel to the server.
 * Returns the number of events processed. Inline payloads only in this slice;
 * arena-payload delivery is a separate slice-4/6 integration. */
uint32_t bus_host_pump(bus_host_t *h);

#endif /* AIMEE_BUS_HOST_H */
