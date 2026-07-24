#include "modules/vault/vault_custody_kms.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
   if (!getenv("AIMEE_VAULT_KMS_HELPER") || !getenv("AIMEE_VAULT_KMS_HWM_PUBKEY") ||
       !getenv("AIMEE_VAULT_KMS_HWM_DOMAIN") || !getenv("AIMEE_VAULT_KMS_KEY_ID"))
   {
      puts("SKIP: signed KMS HWM environment unavailable");
      return 0;
   }
   const vault_custody_provider_t *p = vault_custody_kms_provider();
   uint8_t att[64];
   size_t len = 0;
   uint64_t before = 0, after = 0;
   assert(p && p->hwm_read && p->hwm_cas);
   int saved_stdio[3];
   for (int fd = 0; fd < 3; ++fd)
   {
      saved_stdio[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      assert(saved_stdio[fd] >= 3);
   }
   for (int fd = 0; fd < 3; ++fd)
      assert(close(fd) == 0);
   int closed_stdio_result =
       p->hwm_read(p->ctx, getenv("AIMEE_VAULT_KMS_KEY_ID"), &before, att, sizeof(att), &len);
   for (int fd = 0; fd < 3; ++fd)
   {
      assert(dup2(saved_stdio[fd], fd) == fd);
      assert(close(saved_stdio[fd]) == 0);
   }
   assert(closed_stdio_result == 0);
   assert(before > 0 && len == 64 && before != UINT64_MAX);
   assert(p->hwm_cas(p->ctx, getenv("AIMEE_VAULT_KMS_KEY_ID"), before, before + 1, att, sizeof(att),
                     &len) == 0);
   assert(len == 64);
   assert(p->hwm_cas(p->ctx, getenv("AIMEE_VAULT_KMS_KEY_ID"), before, before + 1, att, sizeof(att),
                     &len) == -1);
   assert(p->hwm_read(p->ctx, getenv("AIMEE_VAULT_KMS_KEY_ID"), &after, att, sizeof(att), &len) ==
          0);
   assert(after == before + 1);
   setenv("AIMEE_TEST_HWM_FORGE", "1", 1);
   assert(p->hwm_read(p->ctx, getenv("AIMEE_VAULT_KMS_KEY_ID"), &after, att, sizeof(att), &len) ==
          -1);
   unsetenv("AIMEE_TEST_HWM_FORGE");
   puts("PASS: signed KMS HWM read/CAS/stale/forgery");
   return 0;
}
