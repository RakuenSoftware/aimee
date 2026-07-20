#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../modules/vault/vault_custody_kms.h"

int main(void)
{
   const char *path = "/tmp/aimee-kms-test-helper";
   FILE *f = fopen(path, "w");
   assert(f);
   fputs("#!/bin/sh\nprintf '01234567890123456789012345678901'\n", f);
   fclose(f);
   assert(chmod(path, 0700) == 0);
   setenv("AIMEE_VAULT_KMS_HELPER", path, 1);
   setenv("AIMEE_VAULT_KMS_KEY_ID", "test-key", 1);
   const vault_custody_provider_t *p = vault_custody_kms_provider();
   uint8_t k[VAULT_KEK_LEN];
   assert(p && p->unseal(p->ctx, NULL, 0) == 0);
   assert(p->get_kek(p->ctx, k) == 0 && k[0] == '0' && k[31] == '1');
   f = fopen(path, "w");
   assert(f);
   fputs("#!/bin/sh\nprintf short\n", f);
   fclose(f);
   assert(p->get_kek(p->ctx, k) != 0);
   remove(path);
   puts("vault_kms: ok");
   return 0;
}
