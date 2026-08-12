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
#include "runtime_secret.h"
#include <stdio.h>
#include <string.h>

char test_vault_server_codex_oauth[4096];

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

/* Presence mirror of the getter below: routability probing asks whether an entry
 * exists WITHOUT decrypting or auditing, so the stub answers from the same
 * fixture the getter uses. Keeping them consistent means a test that seeds a
 * credential sees it as both present and gettable. */
int vault_service_has_server_principal(const char *agent, const char *cred)
{
   return agent && cred && strcmp(agent, "codex") == 0 && strcmp(cred, "oauth") == 0 &&
          test_vault_server_codex_oauth[0];
}

vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   if (agent && cred && strcmp(agent, "codex") == 0 && strcmp(cred, "oauth") == 0 &&
       test_vault_server_codex_oauth[0])
   {
      snprintf(out, out_len, "%s", test_vault_server_codex_oauth);
      return VAULT_OK;
   }
   if (out && out_len)
      out[0] = '\0';
   return VAULT_NO_ENTRY;
}

/* The git ssh-key route (rh_git_sshkey) stores/removes a per-webuser key; tests
 * that link server_http.o but not the real vault get a clean success here. */
vault_status_t vault_service_set(const char *principal, const char *agent, const char *cred,
                                 const char *secret, long now_epoch)
{
   (void)principal;
   (void)agent;
   (void)cred;
   (void)secret;
   (void)now_epoch;
   return VAULT_OK;
}

/* rh_git_sshkey now seals the key under the server KEK (no unlock); same clean
 * success for tests linking server_http.o without the real vault. */
vault_status_t vault_service_set_server_wrap(const char *principal, const char *agent,
                                             const char *cred, const char *secret)
{
   (void)principal;
   (void)agent;
   (void)cred;
   (void)secret;
   return VAULT_OK;
}

vault_status_t vault_service_delete(const char *principal, const char *agent, const char *cred)
{
   (void)principal;
   (void)agent;
   (void)cred;
   return VAULT_OK;
}

/* Tests using this keyless Vault double still exercise the production facade's
 * process-cache contract. Persistence is intentionally represented by the
 * in-memory cache; encrypted-store behavior has its own Vault unit tests. */
int vault_runtime_secret_set(const char *name, const char *value)
{
   return runtime_secret_store(name, value);
}

int vault_runtime_secret_delete(const char *name)
{
   runtime_secret_remove(name);
   return 0;
}
