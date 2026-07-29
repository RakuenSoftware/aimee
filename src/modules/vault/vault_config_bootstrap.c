/* vault_config_bootstrap.c — migrate legacy plaintext config credentials. */
#include "vault_config_bootstrap.h"

#include "config.h"
#include "log.h"
#include "runtime_secret.h"
#include "vault_service.h"
#include "vault_store.h"

#include <openssl/crypto.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_SECRET_AGENT "environment"
#define CONFIG_SECRET_MAX   4096

typedef struct
{
   const char *name;
   size_t offset;
   size_t capacity;
} config_secret_field_t;

#define CONFIG_SECRET_FIELD(name_, field_)                                                        \
   {                                                                                              \
      (name_), offsetof(config_t, field_), sizeof(((config_t *)0)->field_)                        \
   }

static const config_secret_field_t g_fields[] = {
    CONFIG_SECRET_FIELD("AIMEE_DB2_URL", db2_url),
    CONFIG_SECRET_FIELD("AIMEE_SEARCH_TAVILY_API_KEY", search_tavily_api_key),
    CONFIG_SECRET_FIELD("AIMEE_PROXY_TOKEN", proxy_token),
    CONFIG_SECRET_FIELD("AIMEE_INGRESS_PROXY_SECRET", ingress_trusted_proxy_secret),
    CONFIG_SECRET_FIELD("AIMEE_KB_API_BEARER_TOKEN", kb_api_bearer_token),
    CONFIG_SECRET_FIELD("AIMEE_TELEMETRY_METRICS_TOKEN", telemetry_metrics_token),
    CONFIG_SECRET_FIELD("AIMEE_KB_API_BEARER_TOKEN", kb_client_bearer_token),
    CONFIG_SECRET_FIELD("AIMEE_TRIGGER_AUTH_TOKEN", trigger_auth_token),
    CONFIG_SECRET_FIELD("AIMEE_KB_CURATOR_PROVIDER_API_KEY", kb_curator_provider_api_key),
    CONFIG_SECRET_FIELD("AIMEE_KB_CURATOR_TIER_B_API_KEY", kb_curator_tier_b_api_key),
    CONFIG_SECRET_FIELD("AIMEE_API_BEARER_TOKEN", server_api_bearer_token),
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
   vault_status_t st =
       vault_service_delete(VAULT_SERVER_PRINCIPAL, CONFIG_SECRET_AGENT, name);
   if (st != VAULT_OK && st != VAULT_NO_ENTRY)
      return -1;
   runtime_secret_remove(name);
   return 0;
}

static int load_runtime(const char *name)
{
   char value[CONFIG_SECRET_MAX];
   vault_status_t st = vault_service_get_server_principal(CONFIG_SECRET_AGENT, name, value,
                                                           sizeof(value));
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

static int migrate_value(const char *name, char *value, size_t capacity, int *scrubbed)
{
   if (value[0])
   {
      if (!vault_store_has_entry(VAULT_SERVER_PRINCIPAL, CONFIG_SECRET_AGENT, name) &&
          vault_runtime_secret_set(name, value) != 0)
         return -1;
      OPENSSL_cleanse(value, capacity);
      *scrubbed = 1;
   }
   return load_runtime(name);
}

int vault_config_bootstrap_init(void)
{
   /* Register before reading the legacy file: all later generated config
    * credential setters are Vault-only, including during startup callbacks. */
   config_secret_writer_set(config_vault_writer);
   config_t cfg;
   if (config_load_file(&cfg) != 0)
      return -1;

   /* Two historical fields shared one effective KB bearer. Refuse an
    * ambiguous migration instead of silently choosing whichever struct field
    * happens to appear first. A value already supplied through Vault remains
    * authoritative and makes both legacy copies safe to scrub. */
   if (cfg.kb_api_bearer_token[0] && cfg.kb_client_bearer_token[0] &&
       strcmp(cfg.kb_api_bearer_token, cfg.kb_client_bearer_token) != 0 &&
       !vault_store_has_entry(VAULT_SERVER_PRINCIPAL, CONFIG_SECRET_AGENT,
                              "AIMEE_KB_API_BEARER_TOKEN"))
   {
      LOG_ERROR("vault.config", "conflicting legacy KB bearer credentials; migration refused");
      return -1;
   }

   int scrubbed = 0;
   for (size_t i = 0; i < sizeof(g_fields) / sizeof(g_fields[0]); i++)
   {
      char *value = (char *)&cfg + g_fields[i].offset;
      if (migrate_value(g_fields[i].name, value, g_fields[i].capacity, &scrubbed) != 0)
         return -1;
   }

   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (migrate_value(name, cfg.server_api_bearer_extra[i],
                        sizeof(cfg.server_api_bearer_extra[i]), &scrubbed) != 0)
         return -1;
   }
   if (cfg.server_api_bearer_extra_count)
   {
      cfg.server_api_bearer_extra_count = 0;
      scrubbed = 1;
   }

   if (scrubbed && config_save(&cfg) != 0)
   {
      LOG_ERROR("vault.config", "failed to remove migrated credentials from aimee.yaml");
      return -1;
   }
   if (scrubbed)
      LOG_INFO("vault.config", "migrated legacy config credentials into Vault");
   return 0;
}
