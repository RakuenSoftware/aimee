#ifndef AIMEE_RUNTIME_SECRET_H
#define AIMEE_RUNTIME_SECRET_H 1

#include <stddef.h>

/* Process-local plaintext cache populated only from the encrypted Vault during
 * bootstrap. Values never enter the process environment or persistent config.
 * The cache is wiped at shutdown; callers receive bounded copies. */
int runtime_secret_store(const char *name, const char *value);
int runtime_secret_get(const char *name, char *out, size_t out_len);
int runtime_secret_has(const char *name);
void runtime_secret_remove(const char *name);
void runtime_secret_wipe(void *ptr, size_t len);
void runtime_secret_clear(void);

#endif /* AIMEE_RUNTIME_SECRET_H */
