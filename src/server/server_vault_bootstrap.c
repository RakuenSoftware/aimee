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
 * Secret source (optional):
 *   1. AIMEE_DELEGATE_KEY_<AGENT> environment variables (the first-boot path;
 *      the suffix is lowercased and matched against agents.json).
 *   2. AIMEE_FORGE_TOKEN — the server forge identity, stored as
 *      (server, git, forge_token). This is first-boot input only.
 *
 * Safety properties:
 *   - No-op when neither source is set — existing deploys are unchanged.
 *   - Idempotent + non-destructive: an already-vaulted (agent, api_key) is left
 *     untouched unless AIMEE_DELEGATE_SECRETS_OVERWRITE is set, so a redeploy
 *     never silently reverts a rotated key.
 *   - A secret naming an agent absent from agents.json fails boot; silently
 *     discarding a first-boot credential would make the deployment unrecoverable.
 *   - Secrets are never written in plaintext to $AIMEE_HOME or logs; in-memory
 *     copies are OPENSSL_cleanse()d and credential env vars are unset after use.
 */
#include "server.h"
#include "config.h"
#include "vault_service.h"
#include "vault_store.h"
#include "vault_capability.h" /* the local root operator's standing grant */
#include "log.h"
#include <openssl/crypto.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Agent-name resolver seam. Production injects an agents.json-backed resolver
 * (server.c); unit tests inject a trivial one — so this module carries no link
 * dependency on the (heavy) agent config layer. A NULL resolver means every
 * agent is treated as unknown (fail-safe: nothing is sealed). */
static aimee_agent_resolver_fn g_resolver = NULL;

void server_vault_bootstrap_set_resolver(aimee_agent_resolver_fn fn)
{
   g_resolver = fn;
}

#define VAULT_BOOTSTRAP_KEY_PREFIX      "AIMEE_DELEGATE_KEY_"
#define VAULT_BOOTSTRAP_OVERWRITE_ENV   "AIMEE_DELEGATE_SECRETS_OVERWRITE"
#define VAULT_BOOTSTRAP_FORGE_TOKEN_ENV "AIMEE_FORGE_TOKEN"
#define VAULT_BOOTSTRAP_FORGE_AGENT     "git"
#define VAULT_BOOTSTRAP_FORGE_CRED      "forge_token"
#define VAULT_BOOTSTRAP_MAX_ENV_VARS    1024
#define VAULT_BOOTSTRAP_OAUTH_MAX       (64 * 1024)

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
static void provision_server_credential(const char *agent, const char *cred, const char *secret,
                                        int overwrite, prov_counts_t *c)
{
   if (!agent || !agent[0] || !cred || !cred[0] || !secret || !secret[0])
      return;
   if (!overwrite && vault_store_has_entry(VAULT_SERVER_PRINCIPAL, agent, cred))
   {
      c->skipped++;
      return;
   }
   vault_status_t st = vault_service_set_server(agent, cred, secret);
   if (st == VAULT_OK)
      c->provisioned++;
   else
   {
      LOG_ERROR("vault.bootstrap", "failed to seal server credential %s/%s: %s", agent, cred,
                vault_status_str(st));
      c->failed++;
   }
}

static int provision_one(const char *name, const char *secret, int overwrite, prov_counts_t *c)
{
   if (!name || !name[0] || !secret || !secret[0])
      return 0;
   char canon[128];
   if (!g_resolver || !g_resolver(name, canon, sizeof(canon)) || !canon[0])
   {
      LOG_WARN("vault.bootstrap", "secret for unknown agent '%s' — skipped (not in agents.json)",
               name);
      c->unknown++;
      c->failed++;
      return -1;
   }
   int before = c->provisioned;
   provision_server_credential(canon, VAULT_API_KEY_CRED, secret, overwrite, c);
   if (c->provisioned > before)
   {
      /* Agent name only — never the secret or a fingerprint of it. */
      LOG_INFO("vault.bootstrap", "provisioned server-vault api_key for agent '%s'", canon);
   }
   return c->failed ? -1 : 0;
}

static void provision_from_env(int overwrite, prov_counts_t *c)
{
   extern char **environ;
   const size_t plen = strlen(VAULT_BOOTSTRAP_KEY_PREFIX);
   int processed = 0;
   for (; processed < VAULT_BOOTSTRAP_MAX_ENV_VARS; processed++)
   {
      char name[128] = "";
      for (char **e = environ; *e; e++)
      {
         if (strncmp(*e, VAULT_BOOTSTRAP_KEY_PREFIX, plen) != 0)
            continue;
         const char *eq = strchr(*e, '=');
         if (!eq)
            continue;
         size_t klen = (size_t)(eq - *e);
         if (klen == plen || klen >= sizeof(name))
         {
            c->failed++;
            return;
         }
         memcpy(name, *e, klen);
         name[klen] = '\0';
         break;
      }
      if (!name[0])
         break;

      const char *val = getenv(name);
      if (val && val[0])
      {
         char agent[128];
         const char *suf = name + plen;
         size_t j = 0;
         for (; suf[j] && j < sizeof(agent) - 1; j++)
            agent[j] = (char)tolower((unsigned char)suf[j]);
         agent[j] = '\0';
         if (provision_one(agent, val, overwrite, c) != 0)
            return;
      }
      unsetenv(name); /* scrub only after successful ingestion */
   }
   if (processed == VAULT_BOOTSTRAP_MAX_ENV_VARS)
      c->failed++;
}

static int remove_legacy_secret_file(const char *path)
{
   int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   int ok = 1;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || lseek(fd, 0, SEEK_SET) < 0)
      ok = 0;
   char zeroes[4096] = {0};
   off_t remaining = ok ? st.st_size : 0;
   while (remaining > 0)
   {
      size_t want = remaining > (off_t)sizeof(zeroes) ? sizeof(zeroes) : (size_t)remaining;
      ssize_t n = write(fd, zeroes, want);
      if (n <= 0)
      {
         ok = 0;
         break;
      }
      remaining -= n;
   }
   if (ok && (ftruncate(fd, 0) != 0 || fsync(fd) != 0))
      ok = 0;
   if (close(fd) != 0)
      ok = 0;
   if (!ok)
      return -1;
   return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

/* Older images let vendor CLIs retain OAuth JSON under persistent HOME. Seal
 * those opaque documents into the same slots used by the current login flow,
 * then remove them before normal startup. */
static void migrate_legacy_oauth_path(const char *agent, const char *cred, const char *path,
                                      prov_counts_t *c)
{
   int failed_before = c->failed;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
   {
      if (errno != ENOENT)
         c->failed++;
      return;
   }
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
       st.st_size >= VAULT_BOOTSTRAP_OAUTH_MAX)
   {
      close(fd);
      c->failed++;
      return;
   }
   char *secret = malloc((size_t)st.st_size + 1);
   if (!secret)
   {
      close(fd);
      c->failed++;
      return;
   }
   size_t used = 0;
   while (used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, secret + used, (size_t)st.st_size - used);
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   close(fd);
   secret[used] = '\0';
   if (used != (size_t)st.st_size)
      c->failed++;
   else
   {
      while (used > 0 && (secret[used - 1] == '\n' || secret[used - 1] == '\r'))
         secret[--used] = '\0';
      if (used == 0)
         c->failed++;
   }
   if (c->failed == failed_before && !vault_store_has_entry(VAULT_SERVER_PRINCIPAL, agent, cred))
      provision_server_credential(agent, cred, secret, 0, c);
   else if (c->failed == failed_before)
      c->skipped++;

   OPENSSL_cleanse(secret, (size_t)st.st_size + 1);
   free(secret);
   if (c->failed == failed_before && remove_legacy_secret_file(path) != 0)
      c->failed++;
}

/* Older OAuth support also used db1/secrets, whose container fallback is one
 * plaintext file per `oauth.<client>.<field>` under config_output_dir(). Sweep
 * every such file at boot, including orphaned clients no longer present in the
 * MCP registry. An unfamiliar oauth.* file fails startup instead of guessing
 * whether it is safe to retain or delete. */
static void migrate_legacy_db1_oauth(prov_counts_t *c)
{
   static const struct
   {
      const char *suffix;
      const char *cred;
   } fields[] = {
       {".access_token", "oauth_access_token"},
       {".refresh_token", "oauth_refresh_token"},
       {".expires_at", "oauth_expires_at"},
       {".reauth_required", "oauth_reauth_required"},
   };
   const char *dir_path = config_output_dir();
   DIR *dir = opendir(dir_path);
   if (!dir)
   {
      if (errno != ENOENT)
         c->failed++;
      return;
   }
   int read_error = 0;
   for (;;)
   {
      errno = 0;
      struct dirent *entry = readdir(dir);
      if (!entry)
      {
         read_error = errno;
         break;
      }
      const char *name = entry->d_name;
      static const char prefix[] = "oauth.";
      if (strncmp(name, prefix, sizeof(prefix) - 1) != 0)
         continue;
      size_t name_len = strlen(name);
      const char *cred = NULL;
      size_t client_len = 0;
      for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      {
         size_t suffix_len = strlen(fields[i].suffix);
         if (name_len > sizeof(prefix) - 1 + suffix_len &&
             strcmp(name + name_len - suffix_len, fields[i].suffix) == 0)
         {
            cred = fields[i].cred;
            client_len = name_len - (sizeof(prefix) - 1) - suffix_len;
            break;
         }
      }
      if (!cred || client_len == 0 || client_len >= 128)
      {
         c->failed++;
         continue;
      }
      char client[128];
      memcpy(client, name + sizeof(prefix) - 1, client_len);
      client[client_len] = '\0';
      char path[1024];
      int path_len = snprintf(path, sizeof(path), "%s/%s", dir_path, name);
      if (path_len <= 0 || (size_t)path_len >= sizeof(path))
      {
         c->failed++;
         continue;
      }
      migrate_legacy_oauth_path(client, cred, path, c);
   }
   if (read_error != 0 || closedir(dir) != 0)
      c->failed++;
}

static void migrate_legacy_oauth(prov_counts_t *c)
{
   const char *home = getenv("AIMEE_HOME");
   if (!home || !home[0])
      home = getenv("HOME");
   if (!home || !home[0])
      return;
   char path[1024];
   if ((size_t)snprintf(path, sizeof(path), "%s/codex-auth.json", home) < sizeof(path))
      migrate_legacy_oauth_path("codex", "oauth", path, c);
   if ((size_t)snprintf(path, sizeof(path), "%s/.codex/auth.json", home) < sizeof(path))
      migrate_legacy_oauth_path("codex", "oauth", path, c);
   if ((size_t)snprintf(path, sizeof(path), "%s/.claude/.credentials.json", home) < sizeof(path))
      migrate_legacy_oauth_path("claude", "oauth", path, c);
   migrate_legacy_db1_oauth(c);
}

/* The local root operator holds vault:write:server from first boot.
 *
 * A kernel-attested UDS peer running as root is the machine's owner: it already
 * owns .vault/.server-master.key and can decrypt the whole store offline, so the
 * grant confers nothing it lacked. Without it `aimee vault set-server` refuses
 * every root caller -- the ordinary case inside the server container -- and the
 * operator must discover and run a capability grant before storing a single
 * credential. That is the manual step this removes.
 *
 * Recorded rather than special-cased in the gate so it is VISIBLE in `aimee vault
 * capability list` and revocable by an operator who wants it gone. Idempotent, so
 * every boot converges and a deliberate revoke is not silently undone within the
 * same run. */
static void grant_local_root_operator(void)
{
   if (vault_capability_has("uid:0"))
      return;
   if (vault_capability_grant("uid:0") != 0)
      LOG_WARN("vault.bootstrap",
               "could not grant vault:write:server to uid:0 (local root operator); "
               "`aimee vault set-server` will refuse root until it is granted by hand");
   else
      LOG_INFO("vault.bootstrap", "granted vault:write:server to uid:0 (local root operator)");
}

int server_vault_bootstrap(void)
{
   grant_local_root_operator();
   const char *forge_token = getenv(VAULT_BOOTSTRAP_FORGE_TOKEN_ENV);
   int have_forge_token = forge_token && forge_token[0];
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
   int overwrite = env_flag(VAULT_BOOTSTRAP_OVERWRITE_ENV);
   prov_counts_t c = {0, 0, 0, 0};
   migrate_legacy_oauth(&c);
   if (c.failed)
      return -1;
   if (!have_env && !have_forge_token)
      return c.provisioned;

   provision_from_env(overwrite, &c);
   if (have_forge_token)
   {
      provision_server_credential(VAULT_BOOTSTRAP_FORGE_AGENT, VAULT_BOOTSTRAP_FORGE_CRED,
                                  forge_token, overwrite, &c);
      unsetenv(VAULT_BOOTSTRAP_FORGE_TOKEN_ENV);
   }

   if (c.provisioned || c.skipped || c.unknown || c.failed)
      LOG_INFO("vault.bootstrap", "vault bootstrap: provisioned=%d skipped=%d unknown=%d failed=%d",
               c.provisioned, c.skipped, c.unknown, c.failed);
   return c.failed ? -1 : c.provisioned;
}
