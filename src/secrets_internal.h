/* db1/secrets_internal.h: private secret backend helpers.
 *
 * Used by db1/secrets.c and platform-specific backend probes. Not for
 * general callers. */
#ifndef DEC_DB1_SECRETS_INTERNAL_H
#define DEC_DB1_SECRETS_INTERNAL_H 1

#include <stddef.h>

#define AIMEE_SECRET_SERVICE_NAME "aimee"

typedef struct
{
   int (*store)(const char *service, const char *key, const char *value);
   int (*load)(const char *service, const char *key, char *buf, size_t len);
   int (*remove)(const char *service, const char *key);
   const char *name;
} db1_secret_backend_t;

const db1_secret_backend_t *db1_secret_backend_detect(const db1_secret_backend_t *file_fallback);

#endif /* DEC_DB1_SECRETS_INTERNAL_H */
