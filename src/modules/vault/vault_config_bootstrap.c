/* vault_config_bootstrap.c — migrate legacy plaintext config credentials. */
#include "vault_config_bootstrap.h"

#include "config.h"
#include "runtime_secret.h"
#include "vault_service.h"

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
    "AIMEE_KB_SERVICE_IDENTITY_TOKEN",
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

int vault_config_bootstrap_init(void)
{
   /* Every credential setter is Vault-only. Config snapshots never contain
    * credentials, so there is no plaintext configuration migration path. */
   config_secret_writer_set(config_vault_writer);

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
   return 0;
}
