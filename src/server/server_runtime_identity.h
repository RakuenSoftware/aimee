#ifndef AIMEE_SERVER_RUNTIME_IDENTITY_H
#define AIMEE_SERVER_RUNTIME_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
   SERVER_RUNTIME_IDENTITY_NO_TEAM = 0,
   SERVER_RUNTIME_IDENTITY_READY = 1,
   SERVER_RUNTIME_IDENTITY_NO_SERVER_ID = -1,
} server_runtime_identity_state_t;

/* Resolve an explicit operator packet or the wizard-installed v2 identity.
 * Explicit fields never mix with managed fields: if either explicit field is
 * present, both must be valid. */
server_runtime_identity_state_t server_runtime_identity_load(char *server_id, size_t server_id_cap,
                                                             int64_t *team_id);

/* Resolve only the stable server selector for registry/management operations
 * that are not team-scoped. An explicit server id remains compatible without
 * requiring the write-tier team packet. */
int server_runtime_server_id_load(char *server_id, size_t server_id_cap);

#endif
