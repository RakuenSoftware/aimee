#ifndef AIMEE_CORE_EVENT_BUS_ATTACH_H
#define AIMEE_CORE_EVENT_BUS_ATTACH_H 1

#include <stddef.h>
#include <stdint.h>

#define BUS_ATTACH_REQ_MAGIC   0x51524241u /* "ABRQ" */
#define BUS_ATTACH_REPLY_MAGIC 0x50524241u /* "ABRP" */

typedef enum
{
   BUS_ATTACH_OK = 0,
   BUS_ATTACH_DENIED_POLICY,
   BUS_ATTACH_DENIED_VERSION,
   BUS_ATTACH_DENIED_NOSLOT,
   BUS_ATTACH_PROTOCOL
} bus_attach_status_t;

typedef struct
{
   uint32_t magic;
   uint16_t wire_version_min;
   uint16_t wire_version_max;
   uint32_t principal_class;
   uint32_t principal_ref;
} bus_attach_request_t;

typedef struct
{
   uint32_t magic;
   uint32_t status;
   uint32_t handle_id;
   uint32_t wire_version;
   uint32_t slot_size;
   uint32_t inline_budget;
   uint32_t queue_capacity;
   uint32_t reserved;
   uint64_t arena_size;
   uint64_t host_epoch;
} bus_attach_reply_t;

/* The daemon admission callback receives the connected attach socket so it can
 * bind the request's opaque principal to an authenticated OS peer identity
 * (for example SO_PEERCRED) before trusting the claimed values. */
typedef bus_attach_status_t (*bus_admit_fn)(void *ctx, int attach_fd,
                                            const bus_attach_request_t *req);

int bus_fd_send(int sock, const void *payload, size_t len, const int *fds, int n);
long bus_fd_recv(int sock, void *payload, size_t len, int *fds, int max_fds, int *n_out);

#endif
