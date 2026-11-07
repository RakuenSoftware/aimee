#ifndef AIMEE_SERVER_NATIVE_CACHE_H
#define AIMEE_SERVER_NATIVE_CACHE_H
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

typedef struct server_native_cache server_native_cache_t;
/* The host must own a running canonical turn and one native child in that turn;
 * sibling executions use their own private session logs. scope is the attested
 * native execution profile, including account/model lineage, never model data. request contains
 * current authoritative system/catalog and the canonical prior history only.
 * Begin verifies the durable source and consumes any resume checkpoint before
 * returning a handle. Missing/mismatched context or prefix selects fresh full
 * adoption; store failure/corruption and an outstanding claim refuse. */
/* Qualified ownership is host-supplied; publish durably records the exact
 * intended claim before its atomic DB registration. epoch "0" preserves the
 * original cache namespace; later epochs isolate every context in the directory. */
typedef struct server_native_cache_owner
{
   const char *binding, *owner, *containment;
   int (*publish)(const cJSON *claim, void *context);
   void *context;
} server_native_cache_owner_t;
server_native_cache_t *server_native_cache_begin_epoch(const char *session, const char *actor,
                                                       const char *turn, const char *harness,
                                                       const char *scope,
                                                       const aimee_request_t *request,
                                                       const char *epoch,
                                                       const server_native_cache_owner_t *owner);
int server_native_cache_epoch_binding(const char *binding, const char *epoch, char out[65]);
cJSON *server_native_cache_claim_json(const server_native_cache_t *cache);
const char *server_native_cache_resume(const server_native_cache_t *cache);
int server_native_cache_history_offset(const server_native_cache_t *cache);
/* Call after the child has exited and the source owner durably completed the
 * canonical turn. Uses that log as the full prefix source. Never call on process
 * success alone: the host must also qualify native history adoption/fidelity. */
int server_native_cache_complete(server_native_cache_t *cache, const char *native_id);
/* Only after the child is known quiescent. Invalidates the cache; retains all
 * canonical source history. Close alone leaves an unresolved claim fail-closed. */
int server_native_cache_abort(server_native_cache_t *cache);
void server_native_cache_close(server_native_cache_t *cache);
#endif
