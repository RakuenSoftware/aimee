#ifndef AIMEE_SERVER_NATIVE_CACHE_LEASE_H
#define AIMEE_SERVER_NATIVE_CACHE_LEASE_H
/* Host-only cache-directory custody, acquired before native HOME creation or
 * SDK launch. A released flock is never evidence that an old child tree died:
 * the durable marker survives crashes and must continue to refuse restart. */
typedef struct server_native_cache_lease server_native_cache_lease_t;
server_native_cache_lease_t *server_native_cache_lease_open(const char *home, const char *sdk,
                                                            const char *session,
                                                            const char *binding, const char *actor,
                                                            const char *turn, char error[512]);
/* clean requires the existing synchronous child shutdown and successful cache
 * completion/abort, or refusal before child launch. Never use after losing the
 * process owner. Closing unclean retains the marker; no cache files are erased. */
int server_native_cache_lease_close(server_native_cache_lease_t *lease, int clean);
#endif
