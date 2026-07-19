#ifndef DEC_KB_CLIENT_INTERNAL_H
#define DEC_KB_CLIENT_INTERNAL_H 1

#include "cJSON.h"
#include <stdint.h>

/* Internal `/v1` transport helpers shared only by kb-client bridge modules.
 * Public server code should use the typed wrappers in kb_client.h instead of
 * constructing arbitrary /v1 calls. */
const char *kb_client_v1_base_url(void);
char *kb_client_v1_post_json(const char *path, cJSON *body, int timeout_ms, int *status_out);
char *kb_client_v1_post_body(const char *path, const char *body, int timeout_ms, int *status_out);
char *kb_client_v1_post_body_with_type(const char *path, const char *body, const char *content_type,
                                       int timeout_ms, int *status_out);
char *kb_client_v1_get_json(const char *path, int timeout_ms, int *status_out);
char *kb_client_query_escape(const char *s);

#endif /* DEC_KB_CLIENT_INTERNAL_H */
