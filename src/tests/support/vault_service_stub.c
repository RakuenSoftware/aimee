/* vault_service_stub.c: shared no-op stubs for the three vault_service read
 * functions that agent_config.o now calls during credential resolution (P4: the
 * vault is the universal credential source). Tests that link agent_config.o but
 * run keyless (no vault) get a clean VAULT_NO_ENTRY miss here, so resolution
 * falls through to the session/literal/env tiers exactly as before — without
 * pulling in the real vault crypto + store chain. Binaries that need real or
 * controllable vault behavior (unit-test-vault-service, unit-test-server-compute)
 * link the real object / their own file-local stubs instead and must NOT also
 * link this TU. */
#include "vault_service.h"
#include <string.h>

vault_status_t vault_service_inject_api_key(const char *principal, const char *agent, char *api_key,
                                            size_t api_key_len, long now_epoch)
{
   (void)principal;
   (void)agent;
   (void)api_key;
   (void)api_key_len;
   (void)now_epoch;
   return VAULT_NO_ENTRY; /* miss -> caller keeps its config/env key */
}

vault_status_t vault_service_get(const char *principal, const char *agent, const char *cred,
                                 char *out, size_t out_cap, long now_epoch)
{
   (void)principal;
   (void)agent;
   (void)cred;
   (void)now_epoch;
   if (out && out_cap)
      out[0] = '\0';
   return VAULT_NO_ENTRY;
}

vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   (void)agent;
   (void)cred;
   if (out && out_len)
      out[0] = '\0';
   return VAULT_NO_ENTRY;
}
