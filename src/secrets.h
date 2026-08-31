/* db1/secrets.h: user-local secret storage facade.
 *
 * Public DB1 API. Callers see only domain operations; backend selection
 * remains private to the implementation. */
#ifndef DEC_DB1_SECRETS_H
#define DEC_DB1_SECRETS_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   int db1_secret_store(const char *key, const char *value);
   int db1_secret_load(const char *key, char *buf, size_t len);
   int db1_secret_remove(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_SECRETS_H */
