#include "modules/vault/vault_kek_check.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int all_zero(const uint8_t *p, size_t len)
{
   uint8_t any = 0;
   for (size_t i = 0; i < len; i++)
      any |= p[i];
   return any == 0;
}

int main(void)
{
   static const uint8_t fixture[VAULT_WRAPPED_DEK_LEN] = {
       0x84, 0xfa, 0x6a, 0xe8, 0x95, 0x5e, 0xc6, 0x98, 0xc7, 0xde, 0xd2, 0xf0, 0x4a, 0x85,
       0x9b, 0xc3, 0xd6, 0xdf, 0x52, 0x82, 0x1a, 0x06, 0xf2, 0x72, 0x7b, 0x04, 0xa4, 0x38,
       0x92, 0x99, 0x53, 0x28, 0xf1, 0x8b, 0xe5, 0x06, 0x50, 0xe5, 0x9a, 0xf3};
   uint8_t kek[VAULT_KEK_LEN], original[VAULT_KEK_LEN], wrong[VAULT_KEK_LEN];
   for (size_t i = 0; i < sizeof(kek); i++)
      kek[i] = (uint8_t)i;
   memcpy(original, kek, sizeof(kek));
   memset(wrong, 0x5a, sizeof(wrong));

   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN];
   assert(vault_kek_check_wrap(kek, wrapped) == 0);
   assert(CRYPTO_memcmp(wrapped, fixture, sizeof(wrapped)) == 0);
   assert(vault_kek_check_verify(kek, wrapped) == 0);
   assert(vault_kek_check_verify(wrong, wrapped) == -1);
   assert(CRYPTO_memcmp(kek, original, sizeof(kek)) == 0);

   for (size_t i = 0; i < sizeof(wrapped); i++)
   {
      wrapped[i] ^= 1;
      assert(vault_kek_check_verify(kek, wrapped) == -1);
      wrapped[i] ^= 1;
   }

   memset(wrapped, 0xa5, sizeof(wrapped));
   assert(vault_kek_check_wrap(NULL, wrapped) == -1);
   assert(all_zero(wrapped, sizeof(wrapped)));
   assert(vault_kek_check_wrap(kek, NULL) == -1);
   assert(vault_kek_check_verify(NULL, fixture) == -1);
   assert(vault_kek_check_verify(kek, NULL) == -1);
   puts("vault_kek_check: all tests passed");
   return 0;
}
