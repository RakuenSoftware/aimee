/* server_vault_bootstrap.c — boot-time delegate-vault provisioning.
 *
 * A freshly stood-up aimee-server comes up with an EMPTY delegate vault, so
 * every delegate / roundtable call fails 401 (delegate credentials resolve
 * vault-first, delegate_credential_retry.c). This pass seals operator-supplied
 * delegate API keys into the server-principal vault at boot, autonomously:
 * vault_service_set_server() derives the server KEK from the master key
 * (.vault/.server-master.key, auto-minted on first run) — no unlock, no attested
 * connection required. The result is that a new server provisions its own
 * delegates with zero manual `aimee vault set`.
 *
 * Secret sources (precedence, both optional):
 *   1. $AIMEE_DELEGATE_SECRETS_FILE — a JSON object { "<agent>": "<key>", ... }
 *      (the Docker-secret / bind-mount path).
 *   2. AIMEE_DELEGATE_KEY_<AGENT> environment variables (the env-only path;
 *      the suffix is lowercased and matched against agents.json).
 *
 * Safety properties:
 *   - No-op when neither source is set — existing deploys are unchanged.
 *   - Idempotent + non-destructive: an already-vaulted (agent, api_key) is left
 *     untouched unless AIMEE_DELEGATE_SECRETS_OVERWRITE is set, so a redeploy
 *     never silently reverts a rotated key.
 *   - A secret naming an agent absent from agents.json warns and is skipped; it
 *     never fails boot.
 *   - Secrets are never written to $AIMEE_HOME or logs; in-memory copies are
 *     OPENSSL_cleanse()d and AIMEE_DELEGATE_KEY_* env vars are unset after use.
 */
#include "server.h"
#include "vault_service.h"
#include "vault_store.h"
#include "log.h"
#include "cJSON.h"
#include <openssl/crypto.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Agent-name resolver seam. Production injects an agents.json-backed resolver
 * (server.c); unit tests inject a trivial one — so this module carries no link
 * dependency on the (heavy) agent config layer. A NULL resolver means every
 * agent is treated as unknown (fail-safe: nothing is sealed). */
static aimee_agent_resolver_fn g_resolver = NULL;

void server_vault_bootstrap_set_resolver(aimee_agent_resolver_fn fn)
{
   g_resolver = fn;
}

#define VAULT_BOOTSTRAP_SECRETS_FILE_ENV "AIMEE_DELEGATE_SECRETS_FILE"
#define VAULT_BOOTSTRAP_KEY_PREFIX       "AIMEE_DELEGATE_KEY_"
#define VAULT_BOOTSTRAP_OVERWRITE_ENV    "AIMEE_DELEGATE_SECRETS_OVERWRITE"
#define VAULT_BOOTSTRAP_MAX_FILE_BYTES   (1u << 20) /* 1 MB cap on the secrets file */
#define VAULT_BOOTSTRAP_MAX_ENV_VARS     64

typedef struct
{
   int provisioned;
   int skipped;
   int unknown;
   int failed;
} prov_counts_t;

/* Truthy unless empty / "0" / "false" / "no". */
static int env_flag(const char *name)
{
   const char *v = getenv(name);
   return v && v[0] && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
          strcasecmp(v, "no") != 0;
}

/* Seal one (agent, secret) under the server principal. The agent must exist in
 * agents.json; the canonical agent->name is what we key the vault by, so a
 * delegate resolves it later regardless of the source's casing. */
static void provision_one(const char *name, const char *secret, int overwrite, prov_counts_t *c)
{
   if (!name || !name[0] || !secret || !secret[0])
      return;
   char canon[128];
   if (!g_resolver || !g_resolver(name, canon, sizeof(canon)) || !canon[0])
   {
      LOG_WARN("vault.bootstrap", "secret for unknown agent '%s' — skipped (not in agents.json)",
               name);
      c->unknown++;
      return;
   }
   if (!overwrite && vault_store_has_entry(VAULT_SERVER_PRINCIPAL, canon, VAULT_API_KEY_CRED))
   {
      c->skipped++;
      return;
   }
   vault_status_t st = vault_service_set_server(canon, VAULT_API_KEY_CRED, secret);
   if (st == VAULT_OK)
   {
      /* Agent name only — never the secret or a fingerprint of it. */
      LOG_INFO("vault.bootstrap", "provisioned server-vault api_key for agent '%s'", canon);
      c->provisioned++;
   }
   else
   {
      LOG_ERROR("vault.bootstrap", "failed to seal api_key for agent '%s': %s", canon,
                vault_status_str(st));
      c->failed++;
   }
}

/* Read a small file fully; caller frees (and should cleanse). NULL on error or
 * over-cap. */
static char *slurp_file(const char *path, size_t *len_out)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || (unsigned long)sz > VAULT_BOOTSTRAP_MAX_FILE_BYTES || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[n] = '\0';
   if (len_out)
      *len_out = n;
   return buf;
}

static void provision_from_file(const char *path, int overwrite, prov_counts_t *c)
{
   size_t len = 0;
   char *buf = slurp_file(path, &len);
   if (!buf)
   {
      LOG_WARN("vault.bootstrap", "secrets file '%s' unreadable or too large — skipped", path);
      return;
   }
   cJSON *root = cJSON_Parse(buf);
   OPENSSL_cleanse(buf, len ? len : 1);
   free(buf);
   if (!root || !cJSON_IsObject(root))
   {
      LOG_WARN("vault.bootstrap", "secrets file '%s' is not a JSON object — skipped", path);
      if (root)
         cJSON_Delete(root);
      return;
   }
   for (cJSON *it = root->child; it; it = it->next)
      if (cJSON_IsString(it) && it->string)
         provision_one(it->string, it->valuestring, overwrite, c);
   /* Cleanse the decoded secret strings before freeing the tree. */
   for (cJSON *it = root->child; it; it = it->next)
      if (cJSON_IsString(it) && it->valuestring)
         OPENSSL_cleanse(it->valuestring, strlen(it->valuestring));
   cJSON_Delete(root);
}

static void provision_from_env(int overwrite, prov_counts_t *c)
{
   extern char **environ;
   const size_t plen = strlen(VAULT_BOOTSTRAP_KEY_PREFIX);
   /* Snapshot matching var NAMES first: unsetenv() mutates environ, so we must
    * not iterate it and modify it at the same time. */
   char names[VAULT_BOOTSTRAP_MAX_ENV_VARS][128];
   int n = 0;
   for (char **e = environ; *e && n < VAULT_BOOTSTRAP_MAX_ENV_VARS; e++)
   {
      if (strncmp(*e, VAULT_BOOTSTRAP_KEY_PREFIX, plen) != 0)
         continue;
      const char *eq = strchr(*e, '=');
      if (!eq)
         continue;
      size_t klen = (size_t)(eq - *e);
      if (klen == plen || klen >= sizeof(names[0]))
         continue; /* empty suffix or oversize name */
      memcpy(names[n], *e, klen);
      names[n][klen] = '\0';
      n++;
   }
   for (int i = 0; i < n; i++)
   {
      const char *val = getenv(names[i]);
      if (val && val[0])
      {
         char agent[128];
         const char *suf = names[i] + plen;
         size_t j = 0;
         for (; suf[j] && j < sizeof(agent) - 1; j++)
            agent[j] = (char)tolower((unsigned char)suf[j]);
         agent[j] = '\0';
         provision_one(agent, val, overwrite, c);
      }
      unsetenv(names[i]); /* scrub the plaintext from the environment */
   }
}

int server_vault_bootstrap(void)
{
   const char *file = getenv(VAULT_BOOTSTRAP_SECRETS_FILE_ENV);
   int have_file = file && file[0];
   int have_env = 0;
   {
      extern char **environ;
      const size_t plen = strlen(VAULT_BOOTSTRAP_KEY_PREFIX);
      for (char **e = environ; *e; e++)
         if (strncmp(*e, VAULT_BOOTSTRAP_KEY_PREFIX, plen) == 0)
         {
            have_env = 1;
            break;
         }
   }
   if (!have_file && !have_env)
      return 0; /* no secret source configured — nothing to do */

   int overwrite = env_flag(VAULT_BOOTSTRAP_OVERWRITE_ENV);
   prov_counts_t c = {0, 0, 0, 0};
   if (have_file)
      provision_from_file(file, overwrite, &c);
   provision_from_env(overwrite, &c);

   if (c.provisioned || c.skipped || c.unknown || c.failed)
      LOG_INFO("vault.bootstrap",
               "delegate vault provisioning: provisioned=%d skipped=%d unknown=%d failed=%d",
               c.provisioned, c.skipped, c.unknown, c.failed);
   return c.provisioned;
}
