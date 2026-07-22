#ifndef KB_HTTP_SERVERS_H
#define KB_HTTP_SERVERS_H

#include "kb_identity.h"
#include "kb/kb_management_health_exchange.h"

#include <stdint.h>

typedef kb_management_health_result_t (*kb_http_servers_health_handler_fn)(void *,
                                                                           const kb_principal_t *,
                                                                           int64_t, const char *);

/* The management runtime is the sole production owner. Registration is only
 * permitted while no owner (or retiring owner) exists. Unregister prevents new
 * borrows and waits for callbacks already in flight before returning. */
int kb_http_servers_health_register(kb_http_servers_health_handler_fn, void *);
int kb_http_servers_health_unregister(kb_http_servers_health_handler_fn, void *);

int kb_http_servers_route(const char *, const char *, const char *, char *, int);
#endif
