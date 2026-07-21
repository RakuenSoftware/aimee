#ifndef AIMEE_VAULT_KEK_CHECK_H
#define AIMEE_VAULT_KEK_CHECK_H

#include "vault_crypto.h"

#include <stdint.h>

int vault_kek_check_wrap(const uint8_t kek[VAULT_KEK_LEN], uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
int vault_kek_check_verify(const uint8_t kek[VAULT_KEK_LEN],
                           const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);

#endif
