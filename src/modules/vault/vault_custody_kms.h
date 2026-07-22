#ifndef AIMEE_VAULT_CUSTODY_KMS_H
#define AIMEE_VAULT_CUSTODY_KMS_H
#include "vault_internal.h"
const vault_custody_provider_t *vault_custody_kms_provider(void);
int vault_custody_kms_hwm_refresh(void);
int vault_custody_kms_hwm_ready(void);
#endif
