#ifndef AIMEE_KB_HTTP_COMMON_H
#define AIMEE_KB_HTTP_COMMON_H
#include <stddef.h>
#include <stdint.h>
struct cJSON;

/* msg is a trusted, fixed error literal, never request content. */
int kb_http_error(char *out, int cap, int status, const char *msg);
/* Takes ownership of root, including on serialization/size failure. */
int kb_http_emit(struct cJSON *root, char *out, int cap, int status);
int kb_http_tenant_status(int rc);
/* No percent decoding; first matching key wins. Values truncate to cap - 1.
 * Returns 1 if present (including empty), 0 if absent; cap 0 writes nothing. */
int kb_http_query_param(const char *qs, const char *key, char *out, size_t cap);
/* Opens team 0 scope for the verified request actor. On failure writes the HTTP
 * status/error and opens no scope. Caller must commit/roll back after success. */
int kb_http_begin_actor_scope(char *out, int cap, int *http_out);
/* Decimal digits only, positive and within INT64_MAX. */
int kb_http_parse_positive_int64(const char *s, int64_t *out);
#endif
