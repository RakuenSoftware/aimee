#ifndef AIMEE_SERVER_ERROR_KIND_H
#define AIMEE_SERVER_ERROR_KIND_H 1

#include <stdint.h>

/* Production registers the runtime-web event-bus adapter before dispatch
 * starts. Tests may inject the isolated module handler through this seam. */
typedef int (*server_error_http_status_provider_fn)(const char *kind, uint32_t *http_status);

void server_error_kind_register_http_status_provider(server_error_http_status_provider_fn provider);

#endif
