/* vault_env_bootstrap.c — first-boot env transport into encrypted Vault. */
#include "vault_env_bootstrap.h"
#include "runtime_secret.h"
#include "vault_service.h"
#include "vault_store.h"
#include "log.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ENV_AGENT             "environment"
#define ENV_NAME_MAX          128
#define ENV_BOOTSTRAP_MAX     1024
#define ENV_VAULT_ENTRY_MAX   1024
#define ENV_SECRET_VALUE_MAX  4096
#define ENV_STREAM_RECORD_MAX (WEBCHAT_SECRET_MAX + ENV_NAME_MAX + 2)
#define ENV_OVERWRITE_CONTROL "AIMEE_VAULT_ENV_OVERWRITE"
#define WEBCHAT_SECRET_MAX    (64 * 1024)
#define WEBCHAT_AGENT         "webchat-login"

static int span_has_suffix(const char *name, size_t name_len, const char *suffix)
{
   size_t sl = suffix ? strlen(suffix) : 0;
   return name && name_len >= sl && memcmp(name + name_len - sl, suffix, sl) == 0;
}

static int span_contains(const char *name, size_t name_len, const char *needle)
{
   size_t nl = needle ? strlen(needle) : 0;
   if (!name || !nl || name_len < nl)
      return 0;
   for (size_t i = 0; i + nl <= name_len; i++)
      if (memcmp(name + i, needle, nl) == 0)
         return 1;
   return 0;
}

static int name_span_is_credential(const char *name, size_t len, int include_delegate)
{
   if (!name || !len)
      return 0;
   /* Delegate keys have an agent-aware canonical bootstrap of their own. */
   if (len >= 19 && memcmp(name, "AIMEE_DELEGATE_KEY_", 19) == 0)
      return include_delegate;
   if ((len == strlen("AIMEE_DB2_URL") && memcmp(name, "AIMEE_DB2_URL", len) == 0) ||
       (len == strlen("AIMEE_VAULT_PKCS11_PIN") &&
        memcmp(name, "AIMEE_VAULT_PKCS11_PIN", len) == 0) ||
       (len == strlen("AIMEE_WEBCHAT_USER") && memcmp(name, "AIMEE_WEBCHAT_USER", len) == 0) ||
       (len == strlen("AIMEE_WEBCHAT_USERS") && memcmp(name, "AIMEE_WEBCHAT_USERS", len) == 0) ||
       (len == strlen("AIMEE_KB_CONN") && memcmp(name, "AIMEE_KB_CONN", len) == 0) ||
       (len == strlen("AIMEE_KB_CLIENT_PAM_USERNAME") &&
        memcmp(name, "AIMEE_KB_CLIENT_PAM_USERNAME", len) == 0) ||
       (len == strlen("AIMEE_GIT_AUTHOR_NAME") &&
        memcmp(name, "AIMEE_GIT_AUTHOR_NAME", len) == 0) ||
       (len == strlen("AIMEE_GIT_AUTHOR_EMAIL") &&
        memcmp(name, "AIMEE_GIT_AUTHOR_EMAIL", len) == 0) ||
       (len == strlen("DATABASE_URL") && memcmp(name, "DATABASE_URL", len) == 0) ||
       (len == strlen("AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE") &&
        memcmp(name, "AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE", len) == 0))
      return 1;
   return span_has_suffix(name, len, "_TOKEN") || span_has_suffix(name, len, "_SECRET") ||
          span_has_suffix(name, len, "_PASSWORD") || span_has_suffix(name, len, "_PRIVATE_KEY") ||
          span_has_suffix(name, len, "_API_KEY") || span_has_suffix(name, len, "_DSN") ||
          span_has_suffix(name, len, "_BEARER") || span_has_suffix(name, len, "_PASS") ||
          span_has_suffix(name, len, "_CREDENTIAL") || span_has_suffix(name, len, "_CREDENTIALS") ||
          span_contains(name, len, "_SECRET_");
}

int vault_env_name_is_credential(const char *name)
{
   return name_span_is_credential(name, name ? strlen(name) : 0, 0);
}

int vault_env_name_is_any_credential(const char *name)
{
   return name_span_is_credential(name, name ? strlen(name) : 0, 1);
}

int vault_env_has_credential_environment(void)
{
   extern char **environ;
   for (char **entry = environ; *entry; entry++)
   {
      const char *eq = strchr(*entry, '=');
      if (!eq || eq == *entry)
         continue;
      size_t len = (size_t)(eq - *entry);
      if (!name_span_is_credential(*entry, len, 1))
         continue;
      /* The commit identity is SEALED like a credential but is not one: a name
       * and an email are published in every commit this server makes. Leave them
       * in the environment so the Go WFE, which the entrypoint starts as a
       * sibling AFTER this scrub, can author its commits with them. The vault
       * copy stays the store of record for the C paths. */
      if ((len == strlen("AIMEE_GIT_AUTHOR_NAME") &&
           memcmp(*entry, "AIMEE_GIT_AUTHOR_NAME", len) == 0) ||
          (len == strlen("AIMEE_GIT_AUTHOR_EMAIL") &&
           memcmp(*entry, "AIMEE_GIT_AUTHOR_EMAIL", len) == 0))
         continue;
      return len < ENV_NAME_MAX ? 1 : -1;
   }
   return 0;
}

int vault_env_print_credential_names(void)
{
   extern char **environ;
   for (char **entry = environ; *entry; entry++)
   {
      const char *eq = strchr(*entry, '=');
      if (!eq || eq == *entry)
         continue;
      size_t len = (size_t)(eq - *entry);
      if (!name_span_is_credential(*entry, len, 1))
         continue;
      int valid = ((*entry)[0] == '_' || ((*entry)[0] >= 'A' && (*entry)[0] <= 'Z') ||
                   ((*entry)[0] >= 'a' && (*entry)[0] <= 'z'));
      for (size_t i = 1; valid && i < len; i++)
         valid = (*entry)[i] == '_' || ((*entry)[i] >= 'A' && (*entry)[i] <= 'Z') ||
                 ((*entry)[i] >= 'a' && (*entry)[i] <= 'z') ||
                 ((*entry)[i] >= '0' && (*entry)[i] <= '9');
      if (!valid || fwrite(*entry, 1, len, stdout) != len || putchar('\n') == EOF)
         return -1;
   }
   return fflush(stdout) == 0 ? 0 : -1;
}

static int env_name_is_identifier(const char *name, size_t len)
{
   if (!name || !len ||
       !(name[0] == '_' || (name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= 'a' && name[0] <= 'z')))
      return 0;
   for (size_t i = 1; i < len; i++)
      if (!(name[i] == '_' || (name[i] >= 'A' && name[i] <= 'Z') ||
            (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9')))
         return 0;
   return 1;
}

int vault_env_import_stream(FILE *input)
{
   if (!input)
      return -1;

   char *record = calloc(1, ENV_STREAM_RECORD_MAX);
   char(*imported)[ENV_NAME_MAX] = calloc(ENV_BOOTSTRAP_MAX, sizeof(*imported));
   if (!record || !imported)
   {
      free(record);
      free(imported);
      return -1;
   }

   size_t used = 0;
   int imported_count = 0;
   int failed = 0;
   for (;;)
   {
      int ch = fgetc(input);
      if (ch == EOF)
      {
         if (ferror(input) || used != 0)
            failed = 1; /* every record must be NUL terminated */
         break;
      }
      if (ch != '\0')
      {
         if (used + 1 >= ENV_STREAM_RECORD_MAX)
         {
            failed = 1;
            break;
         }
         record[used++] = (char)ch;
         continue;
      }
      if (used == 0)
         continue;

      char *eq = memchr(record, '=', used);
      size_t name_len = eq ? (size_t)(eq - record) : used;
      if (!eq)
      {
         failed = 1;
         break;
      }
      if (eq && name_span_is_credential(record, name_len, 1))
      {
         if (name_len == 0 || name_len >= ENV_NAME_MAX ||
             !env_name_is_identifier(record, name_len) || imported_count >= ENV_BOOTSTRAP_MAX)
         {
            failed = 1;
            break;
         }
         record[name_len] = '\0';
         /* Do not overwrite a credential inherited by the helper. The caller
          * must ingest its own environment first; accepting a duplicate here
          * would make the stream's precedence ambiguous. */
         if (getenv(record) != NULL)
         {
            failed = 1;
            break;
         }
         size_t value_len = used - name_len - 1;
         record[used] = '\0';
         if (setenv(record, eq + 1, 1) != 0)
         {
            failed = 1;
            break;
         }
         memcpy(imported[imported_count], record, name_len + 1);
         imported_count++;
         OPENSSL_cleanse(eq + 1, value_len);
      }
      OPENSSL_cleanse(record, used + 1);
      used = 0;
   }

   if (failed)
   {
      for (int i = 0; i < imported_count; i++)
         unsetenv(imported[i]);
      imported_count = -1;
   }
   OPENSSL_cleanse(record, ENV_STREAM_RECORD_MAX);
   OPENSSL_cleanse(imported, ENV_BOOTSTRAP_MAX * sizeof(*imported));
   free(record);
   free(imported);
   return imported_count;
}

typedef struct
{
   const char *label;
   const char *agent;
   const char *cred;
} webchat_export_t;

static int print_vault_record_base64(const webchat_export_t *record)
{
   char *value = calloc(1, WEBCHAT_SECRET_MAX + 1);
   if (!value)
      return -1;
   vault_status_t st = vault_service_get_server_principal(record->agent, record->cred, value,
                                                          WEBCHAT_SECRET_MAX + 1);
   if (st == VAULT_NO_ENTRY)
   {
      free(value);
      return 0;
   }
   size_t len = strlen(value);
   if (st != VAULT_OK || len == 0 || len > WEBCHAT_SECRET_MAX)
   {
      OPENSSL_cleanse(value, WEBCHAT_SECRET_MAX + 1);
      free(value);
      return -1;
   }
   size_t encoded_cap = 4 * ((len + 2) / 3) + 1;
   unsigned char *encoded = malloc(encoded_cap);
   if (!encoded)
   {
      OPENSSL_cleanse(value, WEBCHAT_SECRET_MAX + 1);
      free(value);
      return -1;
   }
   int encoded_len = EVP_EncodeBlock(encoded, (const unsigned char *)value, (int)len);
   int rc =
       encoded_len > 0 && printf("%s\t%.*s\n", record->label, encoded_len, encoded) > 0 ? 0 : -1;
   OPENSSL_cleanse(encoded, encoded_cap);
   OPENSSL_cleanse(value, WEBCHAT_SECRET_MAX + 1);
   free(encoded);
   free(value);
   return rc;
}

int vault_env_print_webchat_bootstrap(void)
{
   static const webchat_export_t records[] = {
       {"user", ENV_AGENT, "AIMEE_WEBCHAT_USER"},
       {"password", ENV_AGENT, "AIMEE_WEBCHAT_PASSWORD"},
       {"users", ENV_AGENT, "AIMEE_WEBCHAT_USERS"},
       {"legacy_primary", WEBCHAT_AGENT, "legacy_primary"},
       {"legacy_hashes", WEBCHAT_AGENT, "legacy_hashes"},
       {"accounts", WEBCHAT_AGENT, "accounts"},
       {"session_hmac", WEBCHAT_AGENT, "session_hmac"},
       {"tls_key", WEBCHAT_AGENT, "tls_key"},
   };
   for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++)
      if (print_vault_record_base64(&records[i]) != 0)
         return -1;
   return fflush(stdout) == 0 ? 0 : -1;
}

static int vault_record_nonempty(const char *agent, const char *cred)
{
   char *value = calloc(1, WEBCHAT_SECRET_MAX + 1);
   if (!value)
      return -1;
   vault_status_t st =
       vault_service_get_server_principal(agent, cred, value, WEBCHAT_SECRET_MAX + 1);
   int present = st == VAULT_OK && value[0] != '\0';
   if (st != VAULT_OK && st != VAULT_NO_ENTRY)
      present = -1;
   OPENSSL_cleanse(value, WEBCHAT_SECRET_MAX + 1);
   free(value);
   return present;
}

int vault_env_check_webchat_bootstrap(void)
{
   int user = vault_record_nonempty(ENV_AGENT, "AIMEE_WEBCHAT_USER");
   int password = vault_record_nonempty(ENV_AGENT, "AIMEE_WEBCHAT_PASSWORD");
   int legacy_primary = vault_record_nonempty(WEBCHAT_AGENT, "legacy_primary");
   int legacy_hashes = vault_record_nonempty(WEBCHAT_AGENT, "legacy_hashes");
   int accounts = vault_record_nonempty(WEBCHAT_AGENT, "accounts");
   if (user < 0 || password < 0 || legacy_primary < 0 || legacy_hashes < 0 || accounts < 0)
      return -1;
   if (user != password)
      return -1;
   return (user && password) || legacy_primary || legacy_hashes || accounts ? 0 : -1;
}

int vault_env_seal_webchat_record(const char *record_name)
{
   if (!record_name ||
       (strcmp(record_name, "legacy_primary") != 0 && strcmp(record_name, "legacy_hashes") != 0 &&
        strcmp(record_name, "accounts") != 0 && strcmp(record_name, "session_hmac") != 0 &&
        strcmp(record_name, "tls_key") != 0))
      return -1;

   char *value = calloc(1, WEBCHAT_SECRET_MAX + 1);
   if (!value)
      return -1;
   size_t len = fread(value, 1, WEBCHAT_SECRET_MAX + 1, stdin);
   int rc = -1;
   if (len > 0 && len <= WEBCHAT_SECRET_MAX && feof(stdin))
   {
      value[len] = '\0';
      rc = vault_service_set_server(WEBCHAT_AGENT, record_name, value) == VAULT_OK ? 0 : -1;
   }
   OPENSSL_cleanse(value, WEBCHAT_SECRET_MAX + 1);
   free(value);
   return rc;
}

static int env_flag(const char *name)
{
   const char *value = getenv(name);
   return value && value[0] && strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
          strcasecmp(value, "no") != 0;
}

static void slot_for_env(const char *name, const char **agent, const char **cred)
{
   if (strcmp(name, "AIMEE_SERVER_TLS_PRIVATE_KEY") == 0)
   {
      *agent = "__pki_server__";
      *cred = VAULT_API_KEY_CRED;
      return;
   }
   if (strcmp(name, "AIMEE_FORGE_TOKEN") == 0)
   {
      *agent = "git";
      *cred = "forge_token";
      return;
   }
   /* The commit identity lives beside the forge token: same agent, same
    * first-boot transport. aimee cannot read the machine's git config (git_ops
    * points GIT_CONFIG_GLOBAL/SYSTEM at /dev/null), so a commit uses this or it
    * has no author at all. */
   if (strcmp(name, "AIMEE_GIT_AUTHOR_NAME") == 0)
   {
      *agent = "git";
      *cred = "author_name";
      return;
   }
   if (strcmp(name, "AIMEE_GIT_AUTHOR_EMAIL") == 0)
   {
      *agent = "git";
      *cred = "author_email";
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

static int env_name_is_webchat_bootstrap(const char *name)
{
   return name &&
          (strcmp(name, "AIMEE_WEBCHAT_USER") == 0 || strcmp(name, "AIMEE_WEBCHAT_PASSWORD") == 0 ||
           strcmp(name, "AIMEE_WEBCHAT_USERS") == 0);
}

static int preload_vault(int include_delegate)
{
   vault_store_entry_t entries[ENV_VAULT_ENTRY_MAX];
   int count = vault_store_list(VAULT_SERVER_PRINCIPAL, entries, ENV_VAULT_ENTRY_MAX);
   if (count < 0)
      return -1;
   for (int i = 0; i < count; i++)
   {
      if (strcmp(entries[i].agent, ENV_AGENT) == 0)
      {
         if (name_span_is_credential(entries[i].cred, strlen(entries[i].cred), include_delegate) &&
             !env_name_is_webchat_bootstrap(entries[i].cred) &&
             cache_slot(entries[i].cred, entries[i].agent, entries[i].cred) < 0)
            return -1;
      }
      else if (strcmp(entries[i].agent, "git") == 0 && strcmp(entries[i].cred, "forge_token") == 0)
      {
         if (cache_slot("AIMEE_FORGE_TOKEN", entries[i].agent, entries[i].cred) < 0)
            return -1;
      }
   }
   return 0;
}

static int vault_env_bootstrap_init_mode(int include_delegate)
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
         if (len == 0)
            continue;
         if (len >= sizeof(name))
         {
            if (name_span_is_credential(*entry, len, include_delegate))
               failed = 1;
            continue;
         }
         memcpy(name, *entry, len);
         name[len] = '\0';
         if (name_span_is_credential(name, len, include_delegate))
            break;
         name[0] = '\0';
      }
      if (failed)
         break;
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

   if (failed || preload_vault(include_delegate) != 0)
   {
      LOG_ERROR("vault.env", "credential environment bootstrap failed closed");
      runtime_secret_clear();
      return -1;
   }
   if (provisioned)
      LOG_INFO("vault.env", "sealed %d first-boot credential environment value(s)", provisioned);
   return provisioned;
}

int vault_env_bootstrap_init(void)
{
   return vault_env_bootstrap_init_mode(0);
}

int vault_env_bootstrap_init_all(void)
{
   return vault_env_bootstrap_init_mode(1);
}
