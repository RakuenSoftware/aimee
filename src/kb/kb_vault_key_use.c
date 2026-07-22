#include "kb_vault_key_use.h"
#include "kb_vault_policy.h"
#include "vault_server_key.h"
#include <aimee/audit/audit_worm.h>
#include "vault_crypto.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int kb_vault_key_use(const char *p, const char *id, const char *op, kb_vault_key_use_fn fn,
                     void *ctx)
{
   if (!p || !id || !op || !fn || !kb_vault_live_keys_allowed())
      return -1;
   uint8_t k[VAULT_KEK_LEN];
   if (vault_server_kek(k) != 0)
      return -1;
   char d[512];
   int n = snprintf(d, sizeof(d), "key_id=%s operation=%s", id, op);
   if (n < 0 || (size_t)n >= sizeof(d) ||
       audit_worm_append("kb", p, "vault.key_use", id, "allow", d) != 0)
   {
      memset(k, 0, sizeof(k));
      return -1;
   }
   int r = fn(k, sizeof(k), ctx);
   memset(k, 0, sizeof(k));
   return r;
}
