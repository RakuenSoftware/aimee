#ifndef AIMEE_KB_VAULT_PROTECTED_SECRET_H
#define AIMEE_KB_VAULT_PROTECTED_SECRET_H

#include <stddef.h>
#include <stdint.h>

/* A secret arena is always page-backed, locked, excluded from dumps, and
 * wiped in children.  Callers must release it on every exit path. */
typedef struct
{
   uint8_t *bytes;
   size_t length;
   size_t capacity;
   size_t mapped_length;
} kb_vault_protected_secret_t;

int kb_vault_protected_secret_open(kb_vault_protected_secret_t *secret, size_t capacity);
int kb_vault_protected_secret_set_length(kb_vault_protected_secret_t *secret, size_t length);
void kb_vault_protected_cleanse(void *data, size_t length);
void kb_vault_protected_secret_close(kb_vault_protected_secret_t *secret);

#endif
