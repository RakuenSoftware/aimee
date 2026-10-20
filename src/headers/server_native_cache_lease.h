#ifndef AIMEE_SERVER_NATIVE_CACHE_LEASE_H
#define AIMEE_SERVER_NATIVE_CACHE_LEASE_H
#include "cJSON.h"
/* Host-only cache-directory custody, acquired before native HOME creation or
 * SDK launch. A released flock is never evidence that an old child tree died:
 * the durable marker survives crashes and must continue to refuse restart. */
typedef struct server_native_cache_lease server_native_cache_lease_t;
server_native_cache_lease_t *server_native_cache_lease_open(const char *home, const char *sdk,
                                                            const char *session,
                                                            const char *binding, const char *actor,
                                                            const char *turn, char error[512]);
/* Acquire may return an exact prior manifest while retaining the exclusive lock.
 * It grants no authority to clear that owner: caller must prove the exact saved
 * containment empty and commit durable reconciliation before rotate/clean close. */
server_native_cache_lease_t *server_native_cache_lease_acquire(const char *home, const char *sdk,
                                                               const char *session,
                                                               const char *binding,
                                                               const char *actor, const char *turn,
                                                               cJSON **prior, char error[512]);
/* Qualified fresh launch: hold the physical lock and an in-memory nonce only.
 * Persist DB reservation first, then update publishes its marker before SDK access. */
server_native_cache_lease_t *server_native_cache_lease_acquire_unpublished(
    const char *home, const char *sdk, const char *session, const char *binding, const char *actor,
    const char *turn, cJSON **prior, char error[512]);
const cJSON *server_native_cache_lease_manifest(const server_native_cache_lease_t *lease);
const char *server_native_cache_lease_owner(const server_native_cache_lease_t *lease);
int server_native_cache_lease_update(server_native_cache_lease_t *lease, const cJSON *manifest);
int server_native_cache_lease_rotate_unpublished(server_native_cache_lease_t *lease,
                                                 const char *turn, char error[512]);
int server_native_cache_lease_rotate(server_native_cache_lease_t *lease, const char *turn,
                                     char error[512]);
/* clean requires the existing synchronous child shutdown and successful cache
 * completion/abort, or refusal before child launch. Never use after losing the
 * process owner. Closing unclean retains the marker; no cache files are erased. */
int server_native_cache_lease_close(server_native_cache_lease_t *lease, int clean);
#endif
