#ifndef AIMEE_VAULT_HWM_H
#define AIMEE_VAULT_HWM_H
#include <stddef.h>
#include <stdint.h>
int vault_hwm_attest_verify(const char *key_id, uint64_t version, const uint8_t *att, size_t len);
#endif
