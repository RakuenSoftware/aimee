#ifndef KB_HTTP_SERVERS_H
#define KB_HTTP_SERVERS_H

#include "kb_identity.h"
#include "kb/kb_management_health_exchange.h"
#include "kb/kb_management_action.h"

#include <stdint.h>

typedef kb_management_health_result_t (*kb_http_servers_health_handler_fn)(void *,
                                                                           const kb_principal_t *,
                                                                           int64_t, const char *);
typedef kb_management_action_result_t (*kb_http_servers_action_handler_fn)(void *,
                                                                           const kb_principal_t *,
                                                                           int64_t, const char *,
                                                                           const char *, size_t);
typedef enum
{
   KB_MANAGEMENT_READ_OK = 0,
   KB_MANAGEMENT_READ_NOT_FOUND,
   KB_MANAGEMENT_READ_DENIED,
   KB_MANAGEMENT_READ_CONFLICT,
   KB_MANAGEMENT_READ_UNAVAILABLE,
   KB_MANAGEMENT_READ_INTEGRITY,
   KB_MANAGEMENT_READ_INVALID
} kb_management_read_result_t;
typedef kb_management_read_result_t (*kb_http_servers_read_handler_fn)(
    void *, const kb_principal_t *, int64_t, const char *, char *, size_t);

/* The management runtime is the sole production owner. Registration is only
 * permitted while no owner (or retiring owner) exists. Unregister prevents new
 * borrows and waits for callbacks already in flight before returning. */
int kb_http_servers_health_register(kb_http_servers_health_handler_fn, void *);
int kb_http_servers_health_unregister(kb_http_servers_health_handler_fn, void *);
int kb_http_servers_action_register(kb_http_servers_action_handler_fn, void *);
int kb_http_servers_action_unregister(kb_http_servers_action_handler_fn, void *);
int kb_http_servers_read_register(kb_http_servers_read_handler_fn, void *);
int kb_http_servers_read_unregister(kb_http_servers_read_handler_fn, void *);

int kb_http_servers_route(const char *, const char *, const char *, char *, int);
int kb_http_servers_route_ex(const char *, const char *, const char *, const char *, size_t, char *,
                             int);
#endif
