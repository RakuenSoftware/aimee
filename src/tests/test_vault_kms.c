#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
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
   int source_fd = open("/dev/null", O_RDONLY);
   assert(source_fd >= 0);
   int leak_fd = fcntl(source_fd, F_DUPFD, 64);
   assert(leak_fd >= 64);
   close(source_fd);
   char leak_check[160];
   snprintf(leak_check, sizeof(leak_check),
            "#!/bin/sh\n[ ! -e /proc/self/fd/%d ] || exit 9\nprintf '01234567890123456789012345678901'\n",
            leak_fd);
   f = fopen(path, "w");
   assert(f);
   fputs(leak_check, f);
   fclose(f);
   assert(chmod(path, 0700) == 0);
   const vault_custody_provider_t *p = vault_custody_kms_provider();
   uint8_t k[VAULT_KEK_LEN];
   assert(p && p->unseal(p->ctx, NULL, 0) == 0);
   assert(p->get_kek(p->ctx, k) == 0 && k[0] == '0' && k[31] == '1');
   close(leak_fd);
   f = fopen(path, "w");
   assert(f);
   fputs("#!/bin/sh\nprintf short\n", f);
   fclose(f);
   assert(p->get_kek(p->ctx, k) != 0);
   remove(path);
   puts("vault_kms: ok");
   return 0;
}
