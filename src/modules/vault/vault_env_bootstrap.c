/* vault_env_bootstrap.c — first-boot env transport into encrypted Vault. */
#include "vault_env_bootstrap.h"
#include "runtime_secret.h"
#include "vault_service.h"
#include "vault_store.h"
#include "log.h"

#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ENV_AGENT             "environment"
#define ENV_NAME_MAX          128
#define ENV_BOOTSTRAP_MAX     1024
#define ENV_VAULT_ENTRY_MAX   1024
#define ENV_SECRET_VALUE_MAX  4096
#define ENV_OVERWRITE_CONTROL "AIMEE_VAULT_ENV_OVERWRITE"

static int has_suffix(const char *name, const char *suffix)
{
   size_t nl = name ? strlen(name) : 0;
   size_t sl = suffix ? strlen(suffix) : 0;
   return nl >= sl && strcmp(name + nl - sl, suffix) == 0;
}

int vault_env_name_is_credential(const char *name)
{
   if (!name || !name[0])
      return 0;
   /* Delegate keys have an agent-aware canonical bootstrap of their own. */
   if (strncmp(name, "AIMEE_DELEGATE_KEY_", 19) == 0)
      return 0;
   if (strcmp(name, "AIMEE_DB2_URL") == 0)
      return 1; /* may contain a database password */
   if (strcmp(name, "AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE") == 0)
      return 1;
   return has_suffix(name, "_TOKEN") || has_suffix(name, "_SECRET") ||
          has_suffix(name, "_PASSWORD") || has_suffix(name, "_PRIVATE_KEY") ||
          has_suffix(name, "_API_KEY") || has_suffix(name, "_DSN");
}

static int env_flag(const char *name)
{
   const char *value = getenv(name);
   return value && value[0] && strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
          strcasecmp(value, "no") != 0;
}

static void slot_for_env(const char *name, const char **agent, const char **cred)
{
   if (strcmp(name, "AIMEE_FORGE_TOKEN") == 0)
   {
      *agent = "git";
      *cred = "forge_token";
      return;
   }
   *agent = ENV_AGENT;
   *cred = name;
}

static int cache_slot(const char *name, const char *agent, const char *cred)
{
   char value[ENV_SECRET_VALUE_MAX];
   vault_status_t st = vault_service_get_server_principal(agent, cred, value, sizeof(value));
   if (st == VAULT_NO_ENTRY)
      return 0;
   if (st != VAULT_OK || !value[0])
   {
      OPENSSL_cleanse(value, sizeof(value));
      return -1;
   }
   int rc = runtime_secret_store(name, value);
   OPENSSL_cleanse(value, sizeof(value));
   return rc == 0 ? 1 : -1;
}

static int preload_vault(void)
{
   vault_store_entry_t entries[ENV_VAULT_ENTRY_MAX];
   int count = vault_store_list(VAULT_SERVER_PRINCIPAL, entries, ENV_VAULT_ENTRY_MAX);
   if (count < 0)
      return -1;
   for (int i = 0; i < count; i++)
   {
      if (strcmp(entries[i].agent, ENV_AGENT) == 0)
      {
         if (vault_env_name_is_credential(entries[i].cred) &&
             cache_slot(entries[i].cred, entries[i].agent, entries[i].cred) < 0)
            return -1;
      }
      else if (strcmp(entries[i].agent, "git") == 0 &&
               strcmp(entries[i].cred, "forge_token") == 0)
      {
         if (cache_slot("AIMEE_FORGE_TOKEN", entries[i].agent, entries[i].cred) < 0)
            return -1;
      }
   }
   return 0;
}

int vault_env_bootstrap_init(void)
{
   extern char **environ;
   int overwrite = env_flag(ENV_OVERWRITE_CONTROL);
   int provisioned = 0;
   int failed = 0;
   int processed = 0;
   for (; processed < ENV_BOOTSTRAP_MAX; processed++)
   {
      char name[ENV_NAME_MAX] = "";
      for (char **entry = environ; *entry; entry++)
      {
         const char *eq = strchr(*entry, '=');
         if (!eq)
            continue;
         size_t len = (size_t)(eq - *entry);
         if (len == 0 || len >= sizeof(name))
            continue;
         memcpy(name, *entry, len);
         name[len] = '\0';
         if (vault_env_name_is_credential(name))
            break;
         name[0] = '\0';
      }
      if (!name[0])
         break;

      const char *value = getenv(name);
      const char *agent = NULL;
      const char *cred = NULL;
      slot_for_env(name, &agent, &cred);
      if (value && value[0] &&
          (overwrite || !vault_store_has_entry(VAULT_SERVER_PRINCIPAL, agent, cred)))
      {
         if (vault_service_set_server(agent, cred, value) == VAULT_OK)
            provisioned++;
         else
         {
            failed++;
            break; /* preserve the source env in this short-lived failed process */
         }
      }
      unsetenv(name);
   }

   /* A bounded loop is a denial-of-service guard, never a truncation policy:
    * reaching it means at least one credential may remain unsealed, so fail the
    * one-shot helper and do not launch any normal service process. */
   if (processed == ENV_BOOTSTRAP_MAX)
      failed++;

   if (failed || preload_vault() != 0)
   {
      LOG_ERROR("vault.env", "credential environment bootstrap failed closed");
      runtime_secret_clear();
      return -1;
   }
   if (provisioned)
      LOG_INFO("vault.env", "sealed %d first-boot credential environment value(s)", provisioned);
   return provisioned;
}
