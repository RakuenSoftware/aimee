/* Production wrappers for the witness signer: fetch the server KEK and delegate to
 * the pure cores in vault_witness_signer.c. Split out so the cores stay
 * unit-testable without the vault_server_key/config dependency this file needs. */
#include "vault_witness_signer.h"

#include <openssl/crypto.h>
#include <string.h>

#include "vault_server_key.h"

int vault_witness_signer_identity(uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN],
                                  uint8_t key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN])
{
   uint8_t kek[VAULT_KEK_LEN];
   if (vault_server_kek(kek) != 0)
   {
      OPENSSL_cleanse(kek, sizeof kek);
      return -1;
   }
   int rc = vault_witness_signer_identity_from_kek(kek, pub, key_id);
   OPENSSL_cleanse(kek, sizeof kek);
   return rc;
}

int vault_witness_checkpoint_sign(vault_witness_checkpoint_t *cp)
{
   if (!cp)
      return -1;
   uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN], key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN];
   if (vault_witness_signer_identity(pub, key_id) != 0)
      return -1;
   memcpy(cp->signer_key_id, key_id, VAULT_WITNESS_SIGNER_KEY_ID_LEN);
   cp->sig_alg = VAULT_WITNESS_SIG_ED25519;
   if (cp->sig_version == 0)
      cp->sig_version = 1;
   OPENSSL_cleanse(pub, sizeof pub);
   OPENSSL_cleanse(key_id, sizeof key_id);

   uint8_t body[512];
   size_t len = 0;
   if (vault_witness_checkpoint_signable(cp, body, sizeof body, &len) != 0)
      return -1;

   uint8_t kek[VAULT_KEK_LEN];
   if (vault_server_kek(kek) != 0)
   {
      OPENSSL_cleanse(kek, sizeof kek);
      OPENSSL_cleanse(body, sizeof body);
      return -1;
   }
   int rc = vault_witness_signer_sign_from_kek(kek, body, len, cp->signature);
   OPENSSL_cleanse(kek, sizeof kek);
   OPENSSL_cleanse(body, sizeof body);
   return rc;
}
