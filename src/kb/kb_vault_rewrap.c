#include "kb_vault_rewrap.h"
#include "kb_vault_policy.h"
#include "vault_store.h"
#include <aimee/audit/audit_worm.h>
#include <stdio.h>
#include <string.h>
int kb_vault_rewrap_principal(const char *actor, const char *principal, const uint8_t old_kek[32],
                              const uint8_t new_kek[32])
{
   if (!actor || !principal || !old_kek || !new_kek || !kb_vault_live_keys_allowed())
      return -1;
   char d[256];
   int n = snprintf(d, sizeof(d), "principal=%s", principal);
   if (n < 0 || (size_t)n >= sizeof(d) ||
       audit_worm_append("kb", actor, "vault.dek_rewrap", principal, "intent", d) != 0)
      return -1;
   int r = vault_store_rekey(principal, old_kek, new_kek);
   snprintf(d, sizeof(d), "principal=%s status=%d", principal, r);
   if (audit_worm_append("kb", actor, "vault.dek_rewrap", principal, r == 0 ? "allow" : "deny",
                         d) != 0)
      return -1;
   return r;
}
