/* vault_config_bootstrap.c — migrate legacy plaintext config credentials. */
#include "vault_config_bootstrap.h"

#include "config.h"
#include "log.h"
#include "runtime_secret.h"
#include "vault_service.h"
#include "vault_store.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_SECRET_AGENT "environment"
#define CONFIG_SECRET_MAX   4096

static const char *g_secret_names[] = {
    "AIMEE_DB2_URL",
    "AIMEE_SEARCH_TAVILY_API_KEY",
    "AIMEE_PROXY_TOKEN",
    "AIMEE_INGRESS_PROXY_SECRET",
    "AIMEE_KB_API_BEARER_TOKEN",
    "AIMEE_TELEMETRY_METRICS_TOKEN",
    "AIMEE_TRIGGER_AUTH_TOKEN",
    "AIMEE_KB_CURATOR_PROVIDER_API_KEY",
    "EMBEDDER_API_KEY",
    "SYNTHESIS_API_KEY",
    "AIMEE_API_BEARER_TOKEN",
};

static int config_vault_writer(const char *name, const char *value)
{
   return value && value[0] ? vault_runtime_secret_set(name, value)
                            : vault_runtime_secret_delete(name);
}

int vault_runtime_secret_set(const char *name, const char *value)
{
   if (!name || !name[0] || !value || !value[0])
      return -1;
   if (vault_service_set_server(CONFIG_SECRET_AGENT, name, value) != VAULT_OK)
      return -1;
   return runtime_secret_store(name, value);
}

int vault_runtime_secret_delete(const char *name)
{
   if (!name || !name[0])
      return -1;
   vault_status_t st = vault_service_delete(VAULT_SERVER_PRINCIPAL, CONFIG_SECRET_AGENT, name);
   if (st != VAULT_OK && st != VAULT_NO_ENTRY)
      return -1;
   runtime_secret_remove(name);
   return 0;
}

static int load_runtime(const char *name)
{
   char value[CONFIG_SECRET_MAX];
   vault_status_t st =
       vault_service_get_server_principal(CONFIG_SECRET_AGENT, name, value, sizeof(value));
   if (st == VAULT_NO_ENTRY)
      return 0;
   if (st != VAULT_OK || !value[0])
   {
      OPENSSL_cleanse(value, sizeof(value));
      return -1;
   }
   int rc = runtime_secret_store(name, value);
   OPENSSL_cleanse(value, sizeof(value));
   return rc;
}

static int config_vault_present(const char *name)
{
   return vault_store_has_entry(VAULT_SERVER_PRINCIPAL, CONFIG_SECRET_AGENT, name);
}

int vault_config_bootstrap_init(void)
{
   /* Register before reading the legacy file: all later generated config
    * credential setters are Vault-only, including during startup callbacks. */
   config_secret_writer_set(config_vault_writer);
   int migrated = config_migrate_legacy_credentials(config_vault_writer, config_vault_present);
   if (migrated < 0)
   {
      LOG_ERROR("vault.config", "legacy config credential migration failed");
      return -1;
   }

   for (size_t i = 0; i < sizeof(g_secret_names) / sizeof(g_secret_names[0]); i++)
      if (load_runtime(g_secret_names[i]) != 0)
         return -1;

   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (load_runtime(name) != 0)
         return -1;
   }
   if (migrated)
      LOG_INFO("vault.config", "migrated legacy config credentials into Vault");
   return 0;
}
