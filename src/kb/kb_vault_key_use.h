#ifndef AIMEE_KB_VAULT_KEY_USE_H
#define AIMEE_KB_VAULT_KEY_USE_H

#include "kb_identity.h"

#include <stddef.h>
#include <stdint.h>

typedef enum
{
   KB_VAULT_KEY_USE_OK = 0,
   KB_VAULT_KEY_USE_RETRY,
   KB_VAULT_KEY_USE_SEALED,
   KB_VAULT_KEY_USE_UNATTESTED,
   KB_VAULT_KEY_USE_INTEGRITY,
   KB_VAULT_KEY_USE_REPLAY,
   KB_VAULT_KEY_USE_CALLBACK_FAILED,
} kb_vault_key_use_status_t;

/* The plaintext pointer is valid only for the duration of this synchronous call. */
typedef int (*kb_vault_key_use_fn)(const unsigned char *plaintext, size_t plaintext_len, void *ctx);

kb_vault_key_use_status_t
kb_vault_key_use(const kb_principal_t *caller, int64_t team_id,
                 const kb_principal_t *authenticated_origin, const char *use_id, const char *key_id,
                 const char *principal, const char *agent, const char *cred,
                 const char *request_digest, const char *provider, const char *model,
                 const char *operation, kb_vault_key_use_fn callback, void *callback_ctx);

#endif
