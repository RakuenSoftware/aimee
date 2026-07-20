#include <assert.h>
#include <stdint.h>
#include "vault_internal.h"
#include "vault_crypto.h"
#include "vault_custody_pkcs11.h"
int main(void)
{
   const vault_custody_provider_t *p = vault_custody_pkcs11_provider();
   uint8_t kek[VAULT_KEK_LEN] = {0};
   assert(p && p->is_sealed && p->get_kek);
   assert(p->unseal(p->ctx, NULL, 0) == 0);
   assert(p->is_sealed(p->ctx) == 0);
   assert(p->get_kek(p->ctx, kek) == 0);
   int nonzero = 0;
   for (size_t i = 0; i < sizeof(kek); ++i)
      nonzero |= kek[i] != 0;
   assert(nonzero);
   p->seal(p->ctx);
   assert(p->is_sealed(p->ctx) == 1);
   return 0;
}
