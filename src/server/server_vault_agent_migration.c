/* server_vault_agent_migration.c — bind agents.json to first-boot Vault ingestion. */
#include "server.h"

#include "agent_config.h"
#include "vault_service.h"
#include "vault_store.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <unistd.h>

/* Validate first-boot delegate variables against the durable roster and return
 * its canonical spelling. The registry accessor reads the cached registry in
 * place and copies out one agent; the old agent_load_config here was "cached"
 * but still memset and memcpy'd 343 KB per call. */
static int server_bootstrap_resolve_agent(const char *name, char *canon, size_t cap)
{
   agent_t agent;
   if (agent_registry_find(name, &agent) != 0)
      return 0;
   snprintf(canon, cap, "%s", agent.name);
   return 1;
}

/* Canonicalize legacy agents.json credentials into per-agent Vault slots. A
 * $VAR is only first-boot transport: vault_env_bootstrap_init() has already
 * sealed and unset it and populated the locked runtime cache, so agent loading
 * resolves it without consulting getenv(). Once sealed in the agent slot, the
 * reference is removed too. An unresolved reference contains no credential and
 * is safe to retain for a later first boot. */
static int server_migrate_agent_config_credentials(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
   {
      /* A pristine first boot may not have generated agents.json. A present but
       * unreadable registry still fails closed because it may hold plaintext. */
      if (access(agent_config_path(), F_OK) != 0 && errno == ENOENT)
         return 0;
      return -1;
   }

   int changed = 0;
   for (int i = 0; i < cfg.agent_count; i++)
   {
      agent_t *agent = &cfg.agents[i];
      const int stored_literal = agent->api_key_disk[0] && agent->api_key_disk[0] != '$';
      const int resolved_reference =
          agent->api_key_disk[0] == '$' && agent->api_key[0] && agent->api_key[0] != '$';
      if (!stored_literal && !resolved_reference)
         continue;
      if (!agent->name[0] || !agent->api_key[0])
      {
         OPENSSL_cleanse(&cfg, sizeof(cfg));
         return -1;
      }

      if (!vault_store_has_entry(VAULT_SERVER_PRINCIPAL, agent->name, VAULT_API_KEY_CRED) &&
          vault_service_set_server(agent->name, VAULT_API_KEY_CRED, agent->api_key) != VAULT_OK)
      {
         OPENSSL_cleanse(&cfg, sizeof(cfg));
         return -1;
      }
      OPENSSL_cleanse(agent->api_key, sizeof(agent->api_key));
      OPENSSL_cleanse(agent->api_key_disk, sizeof(agent->api_key_disk));
      changed = 1;
   }

   if (changed && agent_save_config(&cfg) != 0)
   {
      OPENSSL_cleanse(&cfg, sizeof(cfg));
      return -1;
   }
   OPENSSL_cleanse(&cfg, sizeof(cfg));
   return 0;
}

int server_vault_bootstrap_prepare(void)
{
   server_vault_bootstrap_set_resolver(server_bootstrap_resolve_agent);
   int provisioned = server_vault_bootstrap();
   if (provisioned < 0 || server_migrate_agent_config_credentials() != 0)
      return -1;
   return provisioned;
}
