/* bus_client.h: the C reference bus client.
 *
 * A per-language client library is what a module uses to attach the bus and
 * publish/subscribe/request (suite terminology). This is the C reference
 * implementation; slice 9 is the Go one, and the two — held to the same wire
 * vectors — are what keep the spec honest.
 *
 * The client attaches over the host's SOCK_SEQPACKET channel, receives its three
 * descriptors, and maps the control region (read-only), the arena, and its own
 * queue pair. After that it never touches a socket: it publishes by writing its
 * outbound ring and receives by reading its inbound ring, both zero-copy in the
 * shared mapping.
 *
 * The host and the client are separate processes; the client never drives the
 * host. It writes outbound and reads inbound, and the host's routing thread does
 * the rest. A request therefore completes in two steps a caller pumps at its own
 * pace: send the request, then poll for the reply.
 */
#ifndef AIMEE_BUS_CLIENT_H
#define AIMEE_BUS_CLIENT_H 1

#include <stdint.h>

#include <aimee/core/event_bus/bus_attach.h>
#include <aimee/core/event_bus/bus_region.h>
#include <aimee/core/event_bus/bus_wire.h>

typedef enum
{
   BUS_CLIENT_OK = 0,
   BUS_CLIENT_WOULD_BLOCK, /* the outbound ring is full; retry later */
   BUS_CLIENT_EMPTY,       /* poll found no event */
   BUS_CLIENT_DENIED,      /* attach was refused (see .attach_status) */
   BUS_CLIENT_EPOCH,       /* the host restarted; this client must re-attach */
   BUS_CLIENT_ERR_ARG,
   BUS_CLIENT_ERR_PAYLOAD, /* payload does not fit the inline budget */
   BUS_CLIENT_ERR_OS,      /* a socket/map failure during attach */
   BUS_CLIENT_ERR_PROTOCOL
} bus_client_result_t;

typedef struct
{
   bus_attach_reply_t reply;
   bus_attach_status_t attach_status;
   bus_region_t control, arena, qpair;
   bus_control_t *ctl;
   bus_qpair_t qp;
   uint64_t attached_epoch;
   int have_pending_read; /* an inbound slot handed out by poll, not yet released */
} bus_client_t;

/* A received event. `payload` points into the shared inbound slot and is valid
 * until the next bus_client_poll call — zero-copy read in place. */
typedef struct
{
   bus_frame_t frame;
   const uint8_t *payload;
   uint32_t payload_len;
} bus_event_t;

/* Attach over a connected SOCK_SEQPACKET socket. On OK the client is mapped and
 * ready; on BUS_CLIENT_DENIED the reason is in c->attach_status. */
bus_client_result_t bus_client_attach(int sock, bus_client_t *c);
bus_client_result_t bus_client_attach_as(int sock, bus_client_t *c, uint32_t principal_class,
                                         uint32_t principal_ref);

/* Unmap and forget. Does not close the attach socket (the caller owns it). */
void bus_client_detach(bus_client_t *c);

/* Publish a one-way notification of `kind`. Inline payload only (len must fit the
 * inline budget). Returns BUS_CLIENT_WOULD_BLOCK if the outbound ring is full. */
bus_client_result_t bus_client_publish(bus_client_t *c, uint32_t kind, const void *payload,
                                       uint32_t len);

/* Publish a one-way notification whose payload lives in the arena (D3), for a
 * payload too large for the inline budget. The caller must already hold the
 * lease: allocate it on the host arena (bus_arena_alloc), fill it
 * (bus_arena_fill_ptr), read its generation (bus_arena_ref), then call this to
 * emit the reference frame — no bytes are copied here. The host publishes the
 * lease to the kind's observers and forwards the reference; a co-located consumer
 * reads it in place via bus_arena_read_ptr and releases it. Once sent, the lease
 * belongs to the host: the producer must not touch it again. */
bus_client_result_t bus_client_publish_arena(bus_client_t *c, uint32_t kind, uint32_t lease_id,
                                             uint32_t generation, uint32_t len);

/* Send a correlated request. The reply arrives later as an inbound event with
 * the same correlation; poll for it. */
bus_client_result_t bus_client_request(bus_client_t *c, uint32_t kind, uint64_t correlation,
                                       const void *payload, uint32_t len);
bus_client_result_t bus_client_request_fragment(bus_client_t *c, uint32_t kind,
                                                uint64_t correlation, const void *payload,
                                                uint32_t len, int more);

/* Reply to a request (a serving module). */
bus_client_result_t bus_client_reply(bus_client_t *c, uint32_t kind, uint64_t correlation,
                                     const void *payload, uint32_t len);
bus_client_result_t bus_client_reply_fragment(bus_client_t *c, uint32_t kind,
                                              uint64_t correlation, const void *payload,
                                              uint32_t len, int more);

/* Cancel an outstanding request. */
bus_client_result_t bus_client_cancel(bus_client_t *c, uint32_t kind, uint64_t correlation);

/* Take the next inbound event. Returns BUS_CLIENT_EMPTY if none. The returned
 * payload pointer is valid until the next poll. */
bus_client_result_t bus_client_poll(bus_client_t *c, bus_event_t *out);

/* Write a heartbeat so the host knows this client is alive. `now` is any value
 * that advances over time. */
void bus_client_heartbeat(bus_client_t *c, uint64_t now);

/* 1 if the host has restarted since attach (a bumped epoch) — every handle and
 * mapping is stale and the client must re-attach. */
int bus_client_epoch_changed(const bus_client_t *c);

/* 1 if the host set this client's sticky control_lost flag (a control-class
 * notice could not be delivered even from the reserve). */
int bus_client_control_lost(const bus_client_t *c);

const char *bus_client_result_name(bus_client_result_t r);

#endif /* AIMEE_BUS_CLIENT_H */
