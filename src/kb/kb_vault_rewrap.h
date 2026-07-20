#ifndef AIMEE_KB_VAULT_REWRAP_H
#define AIMEE_KB_VAULT_REWRAP_H
#include <stdint.h>
int kb_vault_rewrap_principal(const char *actor, const char *principal,
                              const uint8_t old_kek[32], const uint8_t new_kek[32]);
#endif
