#ifndef AIMEE_KB_VAULT_KEY_USE_H
#define AIMEE_KB_VAULT_KEY_USE_H
#include <stddef.h>
typedef int (*kb_vault_key_use_fn)(const unsigned char *, size_t, void *);
int kb_vault_key_use(const char *, const char *, const char *, kb_vault_key_use_fn, void *);
#endif
