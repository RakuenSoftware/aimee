#include "vault_kek_check.h"

#include <openssl/crypto.h>
#include <string.h>

static const uint8_t kek_check_sentinel[VAULT_DEK_LEN] = {
    0x61, 0x69, 0x6d, 0x65, 0x65, 0x2d, 0x76, 0x61, 0x75, 0x6c, 0x74, 0x2d, 0x6b, 0x65, 0x6b, 0x2d,
    0x63, 0x68, 0x65, 0x63, 0x6b, 0x2d, 0x76, 0x31, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

int vault_kek_check_wrap(const uint8_t kek[VAULT_KEK_LEN], uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   if (wrapped)
      OPENSSL_cleanse(wrapped, VAULT_WRAPPED_DEK_LEN);
   if (!kek || !wrapped)
      return -1;
   if (vault_dek_wrap(kek, kek_check_sentinel, wrapped) != 0)
   {
      OPENSSL_cleanse(wrapped, VAULT_WRAPPED_DEK_LEN);
      return -1;
   }
   return 0;
}

int vault_kek_check_verify(const uint8_t kek[VAULT_KEK_LEN],
                           const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN])
{
   if (!kek || !wrapped)
      return -1;
   uint8_t plain[VAULT_DEK_LEN];
   int rc = vault_dek_unwrap(kek, wrapped, plain);
   int ok = rc == 0 && CRYPTO_memcmp(plain, kek_check_sentinel, sizeof(plain)) == 0;
   OPENSSL_cleanse(plain, sizeof(plain));
   return ok ? 0 : -1;
}
