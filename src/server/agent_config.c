/* agent_config.c: config loading/saving, agent routing, role checking, auth resolution */
#include "aimee.h"
#include "util.h"
#include "agent_config.h"
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX for the per-turn vault principal */
#include "vault_service.h"   /* vault_service_* : the permanent credential store (P4) */
#include "model_registry.h"
#include "platform_path.h"
#include "provider_cli_adapter.h"
#include "oauth_flow.h" /* oauth_token_store/get : vault-backed auto-refreshing codex token */
#include "cJSON.h"
#include "json_fluent.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "log.h"
#include <fcntl.h>
#include <unistd.h>

int agent_name_valid(const char *name)
{
   if (!name || !name[0])
      return 0;
   size_t len = strlen(name);
   if (len > 48)
      return 0;
   if (!isalnum((unsigned char)name[0]))
      return 0;
   for (size_t i = 0; i < len; i++)
   {
      char c = name[i];
      if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
         return 0;
   }
   return 1;
}

/* Per-turn session id, set from the request in the chat/delegate workers (setter
 * below) and carried in the creds snapshot so a fan-out worker inherits the
 * originating turn's session identity. (Credentials no longer ride this: the
 * permanent vault is the single source — the legacy session-scoped RAM keyring
 * was retired in P4b.) */
static _Thread_local char g_request_session_id[128];

/* --- Config path --- */

const char *agent_config_path(void)
{
   static char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/agents.json", config_default_dir());
   return path;
}

/* --- Env var expansion --- */

static int agent_expand_env_from_vibe_dotenv(const char *name, char *dst, size_t dst_len)
{
   const char *home = getenv("HOME");
   if (!name || !name[0] || !home || !home[0])
      return 0;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.vibe/.env", home);

   FILE *f = fopen(path, "r");
   if (!f)
      return 0;

   char line[MAX_API_KEY_LEN + 128];
   size_t name_len = strlen(name);
   int found = 0;
   while (fgets(line, sizeof(line), f))
   {
      char *p = line;
      if (strncmp(p, "export ", 7) == 0)
         p += 7;
      if (strncmp(p, name, name_len) != 0 || p[name_len] != '=')
         continue;

      char *val = p + name_len + 1;
      size_t len = strlen(val);
      while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r'))
         val[--len] = '\0';
      if (len >= 2 &&
          ((val[0] == '"' && val[len - 1] == '"') || (val[0] == '\'' && val[len - 1] == '\'')))
      {
         val[len - 1] = '\0';
         val++;
      }
      snprintf(dst, dst_len, "%s", val);
      found = 1;
      break;
   }

   fclose(f);
   return found;
}

void agent_expand_env(const char *src, char *dst, size_t dst_len)
{
   if (!src || !src[0])
   {
      dst[0] = '\0';
      return;
   }

   if (src[0] == '$')
   {
      const char *val = getenv(src + 1);
      if (val)
      {
         snprintf(dst, dst_len, "%s", val);
         return;
      }
      if (agent_expand_env_from_vibe_dotenv(src + 1, dst, dst_len))
         return;
   }

   snprintf(dst, dst_len, "%s", src);
}

static const char *const openrouter_env_vars[] = {"OPENROUTER_API_KEY", NULL};
static const char *const mistral_env_vars[] = {"MISTRAL_API_KEY", NULL};
static const char *const anthropic_env_vars[] = {"ANTHROPIC_API_KEY", NULL};
static const char *const gemini_env_vars[] = {"GEMINI_API_KEY", "GOOGLE_API_KEY", NULL};
static const char *const minimax_env_vars[] = {"MINIMAX_API_KEY", NULL};

static int agent_endpoint_is_localish(const char *endpoint)
{
   if (!endpoint || !endpoint[0])
      return 0;
   return strstr(endpoint, "://localhost") || strstr(endpoint, "://127.") ||
          strstr(endpoint, "://0.0.0.0") || strstr(endpoint, "://[::1]") ||
          strstr(endpoint, "://10.") || strstr(endpoint, "://192.168.") ||
          strstr(endpoint, "://172.16.") || strstr(endpoint, "://172.17.") ||
          strstr(endpoint, "://172.18.") || strstr(endpoint, "://172.19.") ||
          strstr(endpoint, "://172.20.") || strstr(endpoint, "://172.21.") ||
          strstr(endpoint, "://172.22.") || strstr(endpoint, "://172.23.") ||
          strstr(endpoint, "://172.24.") || strstr(endpoint, "://172.25.") ||
          strstr(endpoint, "://172.26.") || strstr(endpoint, "://172.27.") ||
          strstr(endpoint, "://172.28.") || strstr(endpoint, "://172.29.") ||
          strstr(endpoint, "://172.30.") || strstr(endpoint, "://172.31.") ||
          strncmp(endpoint, "unix://", 7) == 0 || endpoint[0] == '/';
}

static int agent_default_inject_respond_tool(const agent_t *ag)
{
   if (!ag || !ag->tools_enabled)
      return 0;
   if (strcmp(ag->provider, "ollama") == 0 || strcmp(ag->provider, "llama_native") == 0 ||
       strcmp(ag->provider, "llama-eval") == 0)
      return 1;
   if (strcmp(ag->auth_type, "none") == 0 && agent_endpoint_is_localish(ag->endpoint))
      return 1;
   return 0;
}

static const char *const *agent_provider_env_vars(const char *provider)
{
   if (!provider || !provider[0])
      return NULL;
   if (strcmp(provider, "openrouter") == 0)
      return openrouter_env_vars;
   if (strcmp(provider, "mistral") == 0)
      return mistral_env_vars;
   if (strcmp(provider, "anthropic") == 0)
      return anthropic_env_vars;
   if (strcmp(provider, "gemini") == 0)
      return gemini_env_vars;
   if (strcmp(provider, "minimax") == 0)
      return minimax_env_vars;
   return NULL;
}

static void agent_normalize_legacy_claude_cli(agent_t *ag)
{
   if (!ag)
      return;
   if (strcmp(ag->backend, AGENT_BACKEND_PROVIDER_CLI) != 0 || strcmp(ag->cli_kind, "claude") != 0)
      return;

   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   if (!ag->provider[0] || strcmp(ag->provider, "openai") == 0)
      snprintf(ag->provider, sizeof(ag->provider), "%s", "claude");
   if (!ag->auth_type[0] || strcmp(ag->auth_type, "bearer") == 0)
      snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", "none");
   if (!ag->cli_cmd[0] || strcmp(ag->cli_cmd, "claude-p") == 0)
      snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", "claude");
   ag->cli_kind[0] = '\0';
}

static void agent_normalize_builtin_cost_tier(agent_t *ag)
{
   if (!ag)
      return;
   if (strcmp(ag->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 &&
       strcmp(ag->cli_kind, "mistral-plan") == 0 && ag->cost_tier > 0)
      ag->cost_tier = 0;
}

static int agent_provider_requires_credentials(const char *provider)
{
   return agent_provider_env_vars(provider) != NULL;
}

static int agent_api_key_literal(const char *api_key)
{
   return api_key && api_key[0] && api_key[0] != '$';
}

static int agent_provider_env_value(const char *provider, char *dst, size_t dst_len)
{
   const char *const *envs = agent_provider_env_vars(provider);
   if (!envs)
      return 0;
   for (int i = 0; envs[i]; i++)
   {
      const char *value = getenv(envs[i]);
      if (value && value[0])
      {
         if (dst && dst_len > 0)
            snprintf(dst, dst_len, "%s", value);
         return 1;
      }
   }
   return 0;
}

static int agent_vault_get(const char *agent_name, const char *cred, char *out, size_t out_len);

int agent_has_resolvable_credentials(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (!agent_provider_requires_credentials(agent->provider))
      return 1;
   if (agent->auth_cmd[0] || agent_api_key_literal(agent->api_key))
      return 1;
   /* A credential in the permanent vault (this turn's principal or the server
    * principal) makes the agent usable with no on-disk key — the P4 primary path.
    * For codex we probe the TOKEN, not the account id: a vaulted account without a
    * token is NOT a usable credential (the token is what authenticates). */
   {
      char k[MAX_API_KEY_LEN];
      int is_codex = strcmp(agent->auth_type, "codex-oauth") == 0;
      if (agent_vault_get(agent->name, is_codex ? VAULT_CODEX_TOKEN_CRED : VAULT_API_KEY_CRED, k,
                          sizeof(k)))
         return 1;
   }
   for (int i = 0; i < agent->credential_count; i++)
   {
      const char *value = getenv(agent->credentials[i].api_key_env);
      if (value && value[0])
         return 1;
   }
   return agent_provider_env_value(agent->provider, NULL, 0);
}

/* Per-turn Codex OAuth creds supplied by the thin client (see agent_config.h).
 * Thread-local: each chat/delegate turn runs on its own worker thread. */
static _Thread_local char g_request_codex_token[MAX_API_KEY_LEN];
static _Thread_local char g_request_codex_account_id[128];

/* Explicit, actionable reason the current turn's auth resolution failed — set by
 * agent_resolve_auth on a known failure (e.g. codex REAUTH_REQUIRED) so the
 * delegate/chat error path can surface it instead of a generic provider 401 (D6).
 * Thread-local + cleared at the start of each resolve. */
static _Thread_local char g_request_auth_error[256];

const char *agent_request_auth_error(void)
{
   return g_request_auth_error[0] ? g_request_auth_error : NULL;
}

void agent_set_request_session(const char *session_id)
{
   if (session_id && session_id[0])
      snprintf(g_request_session_id, sizeof(g_request_session_id), "%s", session_id);
   else
      g_request_session_id[0] = '\0';
}

/* WP-C.2c(3): the attested vault principal for the in-flight chat turn, so a
 * chat-spawned delegate (dispatched through the conn-decoupled agent loop) can
 * reach the originating user's vault. */
static _Thread_local char g_request_vault_principal[VAULT_PRINCIPAL_MAX];

void agent_set_request_vault_principal(const char *principal)
{
   if (principal && principal[0])
      snprintf(g_request_vault_principal, sizeof(g_request_vault_principal), "%s", principal);
   else
      g_request_vault_principal[0] = '\0';
}

const char *agent_get_request_vault_principal(void)
{
   return g_request_vault_principal;
}

/* Per-turn cancellation flag (server-owned turn lifecycle). The chat worker
 * binds a pointer to its turn-registry cancel flag around the in-process agent
 * loop; the loop polls agent_request_cancelled() at safe points to abort a
 * detached turn promptly. Thread-local; NULL clears it. */
static _Thread_local atomic_int *g_request_cancel;

void agent_set_request_cancel(atomic_int *flag)
{
   g_request_cancel = flag;
}

int agent_request_cancelled(void)
{
   return g_request_cancel ? atomic_load(g_request_cancel) : 0;
}

/* Read a credential (codex token/account, etc.) for the in-flight turn from the
 * permanent vault: the turn's attested principal first, then the autonomous
 * server principal (the path that serves a thin-client TCP/TLS conn with no
 * per-user identity). Returns 1 + fills out on a hit, 0 on miss/locked/error so
 * the caller falls through to the remaining (legacy/env) tiers.
 *
 * Threat model for the server-principal fallback: the server vault holds the
 * operator's shared default keys, and aimee-server is a SINGLE-owner personal
 * agent (the server bearer already gates every /v1 op), so serving the server
 * key when the turn's principal has no entry is the intended "all agents work
 * for all connections" behavior, not a cross-tenant leak — a per-user
 * (webuser:/uid:) entry OVERRIDES it when present. Every request that reaches
 * here is already bearer-authenticated. */
static int agent_vault_get(const char *agent_name, const char *cred, char *out, size_t out_len)
{
   if (!agent_name || !agent_name[0] || !out || out_len == 0)
      return 0;
   out[0] = '\0';
   const char *principal = agent_get_request_vault_principal();
   if (principal && principal[0] &&
       vault_service_get(principal, agent_name, cred, out, out_len, time(NULL)) == VAULT_OK &&
       out[0])
      return 1;
   if (vault_service_get_server_principal(agent_name, cred, out, out_len) == VAULT_OK && out[0])
      return 1;
   out[0] = '\0';
   return 0;
}

void agent_set_request_codex_creds(const char *token, const char *account_id)
{
   if (token && token[0])
      snprintf(g_request_codex_token, sizeof(g_request_codex_token), "%s", token);
   else
      g_request_codex_token[0] = '\0';
   if (account_id && account_id[0])
      snprintf(g_request_codex_account_id, sizeof(g_request_codex_account_id), "%s", account_id);
   else
      g_request_codex_account_id[0] = '\0';
}

int agent_request_codex_token_present(void)
{
   return g_request_codex_token[0] != '\0';
}

void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   if (!out)
      return;
   snprintf(out->session_id, sizeof(out->session_id), "%s", g_request_session_id);
   snprintf(out->codex_token, sizeof(out->codex_token), "%s", g_request_codex_token);
   snprintf(out->codex_account_id, sizeof(out->codex_account_id), "%s", g_request_codex_account_id);
   snprintf(out->vault_principal, sizeof(out->vault_principal), "%s", g_request_vault_principal);
}

void agent_request_creds_restore(const agent_request_creds_t *creds)
{
   if (!creds)
      return;
   agent_set_request_session(creds->session_id);
   agent_set_request_codex_creds(creds->codex_token, creds->codex_account_id);
   agent_set_request_vault_principal(creds->vault_principal);
}

static void append_header_line(char *buf, size_t buf_len, const char *line)
{
   if (!buf || buf_len == 0 || !line || !line[0])
      return;
   size_t used = strlen(buf);
   if (used > 0 && buf[used - 1] != '\n' && used + 1 < buf_len)
   {
      buf[used++] = '\n';
      buf[used] = '\0';
   }
   if (used < buf_len - 1)
      snprintf(buf + used, buf_len - used, "%s", line);
}

void agent_build_extra_headers(const agent_t *agent, char *buf, size_t buf_len)
{
   if (!buf || buf_len == 0)
      return;
   buf[0] = '\0';
   if (!agent)
      return;

   if (agent->extra_headers[0])
      snprintf(buf, buf_len, "%s", agent->extra_headers);

   if (strcmp(agent->provider, "anthropic") == 0 && !strstr(buf, "anthropic-version:") &&
       !strstr(buf, "Anthropic-Version:"))
      append_header_line(buf, buf_len, "anthropic-version: 2023-06-01");

   if (strcmp(agent->provider, "openrouter") == 0)
   {
      if (!strstr(buf, "HTTP-Referer:"))
         append_header_line(buf, buf_len, "HTTP-Referer: https://github.com/JBailes/aimee");
      if (!strstr(buf, "X-Title:"))
         append_header_line(buf, buf_len, "X-Title: aimee");
   }

   /* Codex (ChatGPT OAuth): inject the headers the codex backend requires from
    * the client-supplied account id (per-turn push, else the session keyring),
    * unless the agent's stored headers already carry it. Keyed on the
    * codex-oauth auth type so it fires regardless of the provider label (the
    * codex adapter's provider is "chatgpt"). Lets a codex agent be configured
    * without server-held creds. */
   if (strcmp(agent->auth_type, "codex-oauth") == 0 && !strstr(buf, "ChatGPT-Account-ID:"))
   {
      char acct[128];
      acct[0] = '\0';
      if (g_request_codex_account_id[0])
         snprintf(acct, sizeof(acct), "%s", g_request_codex_account_id);
      else
         /* miss leaves acct empty (helper guarantees) -> header is skipped below. */
         (void)agent_vault_get(agent->name, VAULT_CODEX_ACCOUNT_CRED, acct, sizeof(acct));
      if (acct[0])
      {
         if (!strstr(buf, "originator:"))
            append_header_line(buf, buf_len, "originator: codex_cli_rs");
         char line[160];
         snprintf(line, sizeof(line), "ChatGPT-Account-ID: %s", acct);
         append_header_line(buf, buf_len, line);
      }
   }
}

/* --- Load/Save config (with mtime cache) --- */

/* The cache is read/written from many threads at once — every delegate
 * dispatch calls agent_load_config, and the parallel autonomy scheduler plus
 * the panel seats dispatch concurrently. An unguarded memcpy of this
 * multi-KB struct while a reloading thread rewrites it is a torn read (and
 * TSan-class UB), so all cache access holds g_agent_config_cache_lock. The
 * lock is NOT held across file I/O parsing — a reloader parses into the
 * caller's buffer first and only then publishes under the lock. */
static agent_config_t g_agent_config_cache;
static struct timespec g_agent_config_mtime;
/* mtime alone is not a safe cache key. It is not monotonic and not always
 * distinct: a rewritten agents.json can land with a timestamp equal to (or
 * older than) the cached one, and the cache then serves stale content forever.
 * Observed live on the tiered appliance filesystem, where a freshly installed
 * agents.json arrived with an mtime ~9h in the past and /v1/agents kept failing
 * until the file was touched. Size and inode are free from the same stat() and
 * make an in-place rewrite detectable. */
static off_t g_agent_config_size;
static ino_t g_agent_config_ino;
static int g_agent_config_cached;
static pthread_mutex_t g_agent_config_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static struct timespec agent_config_stat_mtime(const struct stat *st)
{
   struct timespec ts;
#if defined(__APPLE__)
   ts = st->st_mtimespec;
#elif defined(_WIN32) || defined(_WIN64)
   ts.tv_sec = st->st_mtime;
   ts.tv_nsec = 0;
#elif defined(__linux__)
   ts = st->st_mtim;
#else
   ts.tv_sec = st->st_mtime;
   ts.tv_nsec = 0;
#endif
   return ts;
}

static int agent_config_mtime_eq(const struct timespec *a, const struct timespec *b)
{
   return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

/* A cache hit requires the file to look identical on every cheap axis stat()
 * gives us: same mtime, same size, same inode. mtime alone is spoofable by a
 * same-timestamp rewrite (see g_agent_config_size). */
static int agent_config_stat_eq(const struct stat *st)
{
   struct timespec mt = agent_config_stat_mtime(st);
   return agent_config_mtime_eq(&mt, &g_agent_config_mtime) && st->st_size == g_agent_config_size &&
          st->st_ino == g_agent_config_ino;
}

/* Record the identity of the file whose parsed contents are now cached. */
static void agent_config_stat_remember(const struct stat *st)
{
   g_agent_config_mtime = agent_config_stat_mtime(st);
   g_agent_config_size = st->st_size;
   g_agent_config_ino = st->st_ino;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   const char *path = agent_config_path();

   /* Return cached config if mtime unchanged and caching enabled */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         pthread_mutex_lock(&g_agent_config_cache_lock);
         int hit = g_agent_config_cached && agent_config_stat_eq(&st);
         if (hit)
            memcpy(cfg, &g_agent_config_cache, sizeof(*cfg));
         pthread_mutex_unlock(&g_agent_config_cache_lock);
         if (hit)
            return 0;
      }
   }

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      return -1;
   }

   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return -1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   data[nread] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      return -1;

   /* Default agent */
   cJSON *def = cJSON_GetObjectItem(root, "default_agent");
   if (def && cJSON_IsString(def))
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", def->valuestring);

   /* Fallback chain */
   cJSON *fb = cJSON_GetObjectItem(root, "fallback_chain");
   if (fb && cJSON_IsArray(fb))
   {
      int n = cJSON_GetArraySize(fb);
      if (n > MAX_FALLBACK)
         n = MAX_FALLBACK;
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_GetArrayItem(fb, i);
         if (cJSON_IsString(item))
            snprintf(cfg->fallback_chain[cfg->fallback_count++], MAX_AGENT_NAME, "%s",
                     item->valuestring);
      }
   }

   /* Agents array */
   cJSON *agents = cJSON_GetObjectItem(root, "agents");
   if (agents && cJSON_IsArray(agents))
   {
      int n = cJSON_GetArraySize(agents);
      if (n > MAX_AGENTS)
         n = MAX_AGENTS;

      for (int i = 0; i < n; i++)
      {
         cJSON *a = cJSON_GetArrayItem(agents, i);
         if (!cJSON_IsObject(a))
            continue;

         agent_t *ag = &cfg->agents[cfg->agent_count];
         memset(ag, 0, sizeof(*ag));

         cJSON *v;
         v = cJSON_GetObjectItem(a, "name");
         if (v && cJSON_IsString(v))
            snprintf(ag->name, MAX_AGENT_NAME, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "endpoint");
         if (v && cJSON_IsString(v))
            snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "model");
         if (v && cJSON_IsString(v))
            snprintf(ag->model, MAX_MODEL_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "api_key");
         if (v && cJSON_IsString(v))
         {
            /* Keep the verbatim on-disk form ($VAR ref) for re-save, and resolve
             * a separate runtime copy. Without this, a save would write the
             * expanded secret back to agents.json as plaintext. */
            snprintf(ag->api_key_disk, MAX_API_KEY_LEN, "%s", v->valuestring);
            agent_expand_env(v->valuestring, ag->api_key, MAX_API_KEY_LEN);
         }

         /* Optional credential pool. Each entry is {name, api_key_env};
          * sibling delegates lease distinct entries via the lease pool in
          * delegate_economics. Skipped when "credentials" is missing —
          * the single-token api_key path stays unchanged. */
         {
            cJSON *creds = cJSON_GetObjectItem(a, "credentials");
            if (creds && cJSON_IsArray(creds))
            {
               cJSON *c;
               cJSON_ArrayForEach(c, creds)
               {
                  if (!cJSON_IsObject(c) || ag->credential_count >= MAX_AGENT_CREDENTIALS)
                     continue;
                  cJSON *cname = cJSON_GetObjectItem(c, "name");
                  cJSON *cenv = cJSON_GetObjectItem(c, "api_key_env");
                  if (!cJSON_IsString(cname) || !cJSON_IsString(cenv))
                     continue;
                  agent_credential_t *slot = &ag->credentials[ag->credential_count];
                  snprintf(slot->name, sizeof(slot->name), "%s", cname->valuestring);
                  snprintf(slot->api_key_env, sizeof(slot->api_key_env), "%s", cenv->valuestring);
                  slot->cooldown_until_ms = 0;
                  ag->credential_count++;
               }
            }
         }

         v = cJSON_GetObjectItem(a, "auth_cmd");
         if (v && cJSON_IsString(v))
            snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "auth_type");
         if (v && cJSON_IsString(v))
            snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", v->valuestring);
         if (!ag->auth_type[0])
            snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", "bearer");

         v = cJSON_GetObjectItem(a, "provider");
         if (v && cJSON_IsString(v))
            snprintf(ag->provider, sizeof(ag->provider), "%s", v->valuestring);
         if (!ag->provider[0])
            snprintf(ag->provider, sizeof(ag->provider), "%s", "openai");
         if (strcmp(ag->provider, "openai") == 0 &&
             (strstr(ag->endpoint, "api.minimax.") || strstr(ag->model, "MiniMax-M2")))
            snprintf(ag->provider, sizeof(ag->provider), "%s", "minimax");

         v = cJSON_GetObjectItem(a, "cost_tier");
         if (v && cJSON_IsNumber(v))
            ag->cost_tier = v->valueint;

         v = cJSON_GetObjectItem(a, "max_tokens");
         ag->max_tokens = (v && cJSON_IsNumber(v)) ? v->valueint : AGENT_DEFAULT_MAX_TOKENS;

         v = cJSON_GetObjectItem(a, "timeout_ms");
         if (v && cJSON_IsNumber(v))
         {
            ag->timeout_ms = v->valueint; /* operator's explicit value wins */
         }
         else
         {
            /* No operator timeout: give a reasoning-capable model a higher per-call
             * default so its slow (multi-minute) completions aren't cut off and
             * retried as spurious read failures. Capability lookup is total +
             * offline (same guarantees as the tools_enabled derivation below);
             * an unknown/non-reasoning model keeps the standard default. */
            model_capability_t tmc;
            int reasoning = ag->model[0] && model_capability_get(ag->provider, ag->model, &tmc) &&
                            (tmc.flags & MODEL_CAP_REASONING);
            ag->timeout_ms = reasoning ? AGENT_REASONING_TIMEOUT_MS : AGENT_DEFAULT_TIMEOUT_MS;
         }

         /* "enabled" accepts a boolean or a 0/1 number: hand-edited rosters
          * write `"enabled": 0`, and treating a non-bool as "enabled" silently
          * re-armed agents the operator had switched off (observed live: a
          * disabled agent with no valid key kept winning role routing and
          * 401-looped every implement step). Absent key stays enabled. */
         v = cJSON_GetObjectItem(a, "enabled");
         if (!v)
            ag->enabled = 1;
         else if (cJSON_IsBool(v))
            ag->enabled = cJSON_IsTrue(v);
         else if (cJSON_IsNumber(v))
            ag->enabled = (v->valueint != 0);
         else
            ag->enabled = 1;

         v = cJSON_GetObjectItem(a, "tools_enabled");
         if (v && cJSON_IsBool(v))
         {
            /* Explicit operator setting always wins (force on or off). */
            ag->tools_enabled = cJSON_IsTrue(v);
         }
         else
         {
            /* Absent key: default tools ON (opt-out, not opt-in). An agent is
             * tool-capable unless the operator EXPLICITLY disables it, or unless
             * the backing model is KNOWN to lack tool support. Defaulting an
             * unknown model to OFF silently made new/unregistered agents (e.g.
             * codex/gpt-5.5, provider=chatgpt — absent from the capability table)
             * look tool-INCAPABLE to the routing filter
             * (delegate_filter_route_capabilities), so tool-requiring roles
             * (`code`/`review`/…) rejected them ("no configured model supports
             * required capabilities (caps=tools)") even though the model runs
             * tools fine.
             *
             * Invariants:
             *  - Operator intent is never overridden: an explicit
             *    "tools_enabled": true|false is honoured verbatim (branch above).
             *    This only sets a value the operator left UNSPECIFIED.
             *  - The lookup is total and offline: model_capability_get resolves
             *    via on-disk overrides/caches and a pure in-code heuristic, never
             *    the network. We only force OFF when it POSITIVELY resolves a
             *    model that lacks MODEL_CAP_TOOLS; an unknown/empty model defaults
             *    ON. So embedders/rerankers registered with a known non-tool model
             *    stay off, while any real chat/code delegate is capable by default. */
            model_capability_t mc;
            int known = ag->model[0] && model_capability_get(ag->provider, ag->model, &mc);
            ag->tools_enabled = (known && !(mc.flags & MODEL_CAP_TOOLS)) ? 0 : 1;
         }

         v = cJSON_GetObjectItem(a, "recommended_sampling");
         ag->recommended_sampling = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         v = cJSON_GetObjectItem(a, "inject_respond_tool");
         ag->inject_respond_tool =
             (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : agent_default_inject_respond_tool(ag);

         /* Per-agent delegate turn cap. Declared value wins verbatim:
          *   0  = unlimited (frontier agents, e.g. MiniMax-M3),
          *   >0 = explicit cap,
          *   absent -> -1 = inherit the per-role floor (see delegate_role.c).
          * The role floor never clamps a declared cap down. */
         v = cJSON_GetObjectItem(a, "max_turns");
         ag->max_turns = (v && cJSON_IsNumber(v)) ? v->valueint : -1;

         v = cJSON_GetObjectItem(a, "max_parallel");
         if (v && cJSON_IsNumber(v))
            ag->max_parallel = v->valueint;
         else if (strcmp(ag->provider, "mistral") == 0)
            ag->max_parallel = 2; /* mistral rate limits fire under concurrent load */
         else
            ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

         v = cJSON_GetObjectItem(a, "exec_system_prompt");
         if (v && cJSON_IsString(v))
            snprintf(ag->exec_system_prompt, MAX_EXEC_PROMPT_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "extra_headers");
         if (v && cJSON_IsString(v))
            snprintf(ag->extra_headers, sizeof(ag->extra_headers), "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "fallback_model");
         if (v && cJSON_IsString(v))
            snprintf(ag->fallback_model, sizeof(ag->fallback_model), "%s", v->valuestring);

         cJSON *exec_roles = cJSON_GetObjectItem(a, "exec_roles");
         if (exec_roles && cJSON_IsArray(exec_roles))
         {
            int ern = cJSON_GetArraySize(exec_roles);
            if (ern > MAX_EXEC_ROLES)
               ern = MAX_EXEC_ROLES;
            for (int j = 0; j < ern; j++)
            {
               cJSON *er = cJSON_GetArrayItem(exec_roles, j);
               if (cJSON_IsString(er))
                  snprintf(ag->exec_roles[ag->exec_role_count++], 32, "%s", er->valuestring);
            }
         }

         cJSON *roles = cJSON_GetObjectItem(a, "roles");
         if (roles && cJSON_IsArray(roles))
         {
            int rn = cJSON_GetArraySize(roles);
            if (rn > MAX_AGENT_ROLES)
               rn = MAX_AGENT_ROLES;
            for (int j = 0; j < rn; j++)
            {
               cJSON *r = cJSON_GetArrayItem(roles, j);
               if (cJSON_IsString(r))
                  snprintf(ag->roles[ag->role_count++], 32, "%s", r->valuestring);
            }
         }

         /* Personas this agent may be dispatched AS (delegate identities). An
          * absent/empty list means "all" (agent_supports_persona), so existing
          * agents.json without this key keep serving every persona. */
         cJSON *personas = cJSON_GetObjectItem(a, "personas");
         if (personas && cJSON_IsArray(personas))
         {
            int pn = cJSON_GetArraySize(personas);
            if (pn > MAX_AGENT_PERSONAS)
               pn = MAX_AGENT_PERSONAS;
            for (int j = 0; j < pn; j++)
            {
               cJSON *p = cJSON_GetArrayItem(personas, j);
               if (cJSON_IsString(p))
                  snprintf(ag->personas[ag->persona_count++], 32, "%s", p->valuestring);
            }
         }

         /* Middleware configuration (optional per-agent overrides) */
         cJSON *mw = cJSON_GetObjectItem(a, "middleware");
         if (mw && cJSON_IsObject(mw))
         {
            v = cJSON_GetObjectItem(mw, "cost_limit");
            if (v && cJSON_IsNumber(v))
               ag->middleware.cost_limit = v->valueint;
            v = cJSON_GetObjectItem(mw, "context_warn_pct");
            if (v && cJSON_IsNumber(v))
               ag->middleware.context_warn_pct = v->valueint;
            v = cJSON_GetObjectItem(mw, "auto_compact_pct");
            if (v && cJSON_IsNumber(v))
               ag->middleware.auto_compact_pct = v->valueint;
            v = cJSON_GetObjectItem(mw, "stall_threshold");
            if (v && cJSON_IsNumber(v))
               ag->middleware.stall_threshold = v->valueint;
            v = cJSON_GetObjectItem(mw, "context_window");
            if (v && cJSON_IsNumber(v))
               ag->middleware.context_window = v->valueint;
         }

         /* CLI backend config */
         v = cJSON_GetObjectItem(a, "backend");
         if (v && cJSON_IsString(v))
         {
            if (strcmp(v->valuestring, AGENT_BACKEND_CLI_STDIO) == 0)
               snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
            else
               snprintf(ag->backend, sizeof(ag->backend), "%s", v->valuestring);
         }

         v = cJSON_GetObjectItem(a, "cli_cmd");
         if (v && cJSON_IsString(v))
            snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "cli_idle_timeout_ms");
         if (v && cJSON_IsNumber(v))
            ag->cli_idle_timeout_ms = v->valueint;

         v = cJSON_GetObjectItem(a, "session_reuse");
         ag->session_reuse = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         /* Per-CLI adapter selector for provider-cli backend. Empty when
          * the backend is HTTP or tmux-cli. */
         v = cJSON_GetObjectItem(a, "cli_kind");
         if (v && cJSON_IsString(v))
            snprintf(ag->cli_kind, sizeof(ag->cli_kind), "%s", v->valuestring);
         v = cJSON_GetObjectItem(a, "is_server_hosted");
         if (v && cJSON_IsBool(v))
            ag->is_server_hosted = cJSON_IsTrue(v);
         v = cJSON_GetObjectItem(a, "primary_only");
         if (v && cJSON_IsBool(v))
            ag->primary_only = cJSON_IsTrue(v);

         agent_normalize_legacy_claude_cli(ag);
         agent_normalize_builtin_cost_tier(ag);
         cfg->agent_count++;
      }
   }

   /* Network config */
   cJSON *net = cJSON_GetObjectItem(root, "network");
   if (net && cJSON_IsObject(net))
   {
      agent_network_t *nw = &cfg->network;
      memset(nw, 0, sizeof(*nw));

      cJSON *v;
      v = cJSON_GetObjectItem(net, "ssh_entry");
      if (v && cJSON_IsString(v))
         snprintf(nw->ssh_entry, sizeof(nw->ssh_entry), "%s", v->valuestring);
      v = cJSON_GetObjectItem(net, "ssh_key");
      if (v && cJSON_IsString(v))
         snprintf(nw->ssh_key, sizeof(nw->ssh_key), "%s", v->valuestring);

      cJSON *hosts = cJSON_GetObjectItem(net, "hosts");
      if (hosts && cJSON_IsArray(hosts))
      {
         int hn = cJSON_GetArraySize(hosts);
         if (hn > AGENT_MAX_NET_HOSTS)
            hn = AGENT_MAX_NET_HOSTS;
         for (int i = 0; i < hn; i++)
         {
            cJSON *h = cJSON_GetArrayItem(hosts, i);
            if (!cJSON_IsObject(h))
               continue;
            agent_net_host_t *host = &nw->hosts[nw->host_count];
            v = cJSON_GetObjectItem(h, "name");
            if (v && cJSON_IsString(v))
               snprintf(host->name, sizeof(host->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "ip");
            if (v && cJSON_IsString(v))
               snprintf(host->ip, sizeof(host->ip), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "user");
            if (v && cJSON_IsString(v))
               snprintf(host->user, sizeof(host->user), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "port");
            host->port = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
            v = cJSON_GetObjectItem(h, "desc");
            if (v && cJSON_IsString(v))
               snprintf(host->desc, sizeof(host->desc), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "tunnel");
            if (v && cJSON_IsString(v))
               snprintf(host->tunnel, sizeof(host->tunnel), "%s", v->valuestring);
            nw->host_count++;
         }
      }

      cJSON *networks = cJSON_GetObjectItem(net, "networks");
      if (networks && cJSON_IsArray(networks))
      {
         int nn = cJSON_GetArraySize(networks);
         if (nn > AGENT_MAX_NETWORKS)
            nn = AGENT_MAX_NETWORKS;
         for (int i = 0; i < nn; i++)
         {
            cJSON *n = cJSON_GetArrayItem(networks, i);
            if (!cJSON_IsObject(n))
               continue;
            agent_net_def_t *nd = &nw->networks[nw->network_count];
            v = cJSON_GetObjectItem(n, "name");
            if (v && cJSON_IsString(v))
               snprintf(nd->name, sizeof(nd->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(n, "cidr");
            if (v && cJSON_IsString(v))
               snprintf(nd->cidr, sizeof(nd->cidr), "%s", v->valuestring);
            v = cJSON_GetObjectItem(n, "desc");
            if (v && cJSON_IsString(v))
               snprintf(nd->desc, sizeof(nd->desc), "%s", v->valuestring);
            nw->network_count++;
         }
      }

      /* Tunnel config */
      cJSON *tunnels = cJSON_GetObjectItem(net, "tunnels");
      if (tunnels && cJSON_IsArray(tunnels))
      {
         int tn = cJSON_GetArraySize(tunnels);
         if (tn > AGENT_MAX_TUNNELS)
            tn = AGENT_MAX_TUNNELS;
         agent_tunnel_mgr_t *tmgr = &cfg->tunnel_mgr;
         for (int i = 0; i < tn; i++)
         {
            cJSON *t = cJSON_GetArrayItem(tunnels, i);
            if (!cJSON_IsObject(t))
               continue;
            agent_tunnel_t *tun = &tmgr->tunnels[tmgr->tunnel_count];
            memset(tun, 0, sizeof(*tun));
            v = cJSON_GetObjectItem(t, "name");
            if (v && cJSON_IsString(v))
               snprintf(tun->name, sizeof(tun->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "relay_ssh");
            if (v && cJSON_IsString(v))
               snprintf(tun->relay_ssh, sizeof(tun->relay_ssh), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "relay_key");
            if (v && cJSON_IsString(v))
               snprintf(tun->relay_key, sizeof(tun->relay_key), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "target_host");
            if (v && cJSON_IsString(v))
               snprintf(tun->target_host, sizeof(tun->target_host), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "target_port");
            tun->target_port = (v && cJSON_IsNumber(v)) ? v->valueint : 22;
            v = cJSON_GetObjectItem(t, "reconnect_delay");
            tun->reconnect_delay_s = (v && cJSON_IsNumber(v)) ? v->valueint : 5;
            v = cJSON_GetObjectItem(t, "max_reconnects");
            tun->max_reconnects = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
            tmgr->tunnel_count++;
         }
         if (tmgr->tunnel_count > 0)
            nw->tunnel_mgr = tmgr;
      }
   }

   cJSON_Delete(root);

   /* Update mtime cache */
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         pthread_mutex_lock(&g_agent_config_cache_lock);
         memcpy(&g_agent_config_cache, cfg, sizeof(g_agent_config_cache));
         agent_config_stat_remember(&st);
         g_agent_config_cached = 1;
         pthread_mutex_unlock(&g_agent_config_cache_lock);
      }
   }

   return 0;
}

/* Count the agents in the on-disk agents.json WITHOUT the cache — the cache can be
 * stale or empty, and this guards against destroying the real file. Returns the
 * count, or -1 if the file is absent or unparseable (i.e. "nothing to protect"). */
static int agent_config_existing_agent_count(void)
{
   FILE *f = fopen(agent_config_path(), "r");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return -1;
   }
   long sz = ftell(f);
   if (sz <= 0 || sz > 8 * 1024 * 1024)
   {
      fclose(f);
      return -1;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return -1;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[rd] = '\0';
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return -1;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "agents");
   int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
   cJSON_Delete(root);
   return n;
}

int agent_save_config(const agent_config_t *cfg)
{
   cJSON *root = cJSON_CreateObject();

   if (cfg->default_agent[0])
      JSON_ADD_STR(root, "default_agent", cfg->default_agent);

   cJSON *fb = cJSON_CreateArray();
   for (int i = 0; i < cfg->fallback_count; i++)
      cJSON_AddItemToArray(fb, cJSON_CreateString(cfg->fallback_chain[i]));
   cJSON_AddItemToObject(root, "fallback_chain", fb);

   cJSON *agents = cJSON_CreateArray();
   for (int i = 0; i < cfg->agent_count; i++)
   {
      const agent_t *ag = &cfg->agents[i];
      cJSON *a = cJSON_CreateObject();

      JSON_ADD_STR(a, "name", ag->name);
      JSON_ADD_STR(a, "endpoint", ag->endpoint);
      JSON_ADD_STR(a, "model", ag->model);
      /* Persist the on-disk reference form ($VAR), never a resolved secret. Falls
       * back to api_key for in-memory agents (e.g. `agent add $VAR`, which stores
       * the unexpanded reference there); literal secrets belong in the vault. */
      {
         const char *disk_key = ag->api_key_disk[0] ? ag->api_key_disk : ag->api_key;
         if (disk_key[0])
            JSON_ADD_STR(a, "api_key", disk_key);
      }
      if (ag->auth_cmd[0])
         JSON_ADD_STR(a, "auth_cmd", ag->auth_cmd);
      if (strcmp(ag->auth_type, "bearer") != 0)
         JSON_ADD_STR(a, "auth_type", ag->auth_type);
      if (strcmp(ag->provider, "openai") != 0)
         JSON_ADD_STR(a, "provider", ag->provider);

      cJSON *roles = cJSON_CreateArray();
      for (int j = 0; j < ag->role_count; j++)
         cJSON_AddItemToArray(roles, cJSON_CreateString(ag->roles[j]));
      cJSON_AddItemToObject(a, "roles", roles);

      /* Only serialize personas when explicitly set (empty = "all" implicitly). */
      if (ag->persona_count > 0)
      {
         cJSON *personas = cJSON_CreateArray();
         for (int j = 0; j < ag->persona_count; j++)
            cJSON_AddItemToArray(personas, cJSON_CreateString(ag->personas[j]));
         cJSON_AddItemToObject(a, "personas", personas);
      }

      cJSON_AddNumberToObject(a, "cost_tier", ag->cost_tier);
      cJSON_AddNumberToObject(a, "max_tokens", ag->max_tokens);
      cJSON_AddNumberToObject(a, "timeout_ms", ag->timeout_ms);
      cJSON_AddBoolToObject(a, "enabled", ag->enabled);
      if (ag->recommended_sampling)
         cJSON_AddBoolToObject(a, "recommended_sampling", ag->recommended_sampling);
      if (ag->max_parallel != AGENT_DEFAULT_MAX_PARALLEL)
         cJSON_AddNumberToObject(a, "max_parallel", ag->max_parallel);

      if (ag->tools_enabled)
      {
         cJSON_AddBoolToObject(a, "tools_enabled", ag->tools_enabled);
         if (ag->inject_respond_tool)
            cJSON_AddBoolToObject(a, "inject_respond_tool", ag->inject_respond_tool);
         cJSON_AddNumberToObject(a, "max_turns", ag->max_turns);
         if (ag->exec_system_prompt[0])
            JSON_ADD_STR(a, "exec_system_prompt", ag->exec_system_prompt);
      }
      if (ag->exec_role_count > 0)
      {
         cJSON *er = cJSON_CreateArray();
         for (int j = 0; j < ag->exec_role_count; j++)
            cJSON_AddItemToArray(er, cJSON_CreateString(ag->exec_roles[j]));
         cJSON_AddItemToObject(a, "exec_roles", er);
      }

      if (ag->extra_headers[0])
         JSON_ADD_STR(a, "extra_headers", ag->extra_headers);
      if (ag->fallback_model[0])
         JSON_ADD_STR(a, "fallback_model", ag->fallback_model);
      if (ag->backend[0])
         JSON_ADD_STR(a, "backend", ag->backend);
      if (ag->cli_cmd[0])
         JSON_ADD_STR(a, "cli_cmd", ag->cli_cmd);
      if (ag->cli_idle_timeout_ms > 0)
         cJSON_AddNumberToObject(a, "cli_idle_timeout_ms", ag->cli_idle_timeout_ms);
      if (ag->session_reuse)
         cJSON_AddBoolToObject(a, "session_reuse", ag->session_reuse);
      if (ag->cli_kind[0])
         JSON_ADD_STR(a, "cli_kind", ag->cli_kind);
      if (ag->is_server_hosted)
         cJSON_AddBoolToObject(a, "is_server_hosted", 1);
      if (ag->primary_only)
         cJSON_AddBoolToObject(a, "primary_only", 1);

      /* Middleware config: only write if any non-zero field is set */
      {
         const agent_middleware_cfg_t *mwc = &ag->middleware;
         if (mwc->cost_limit || mwc->context_warn_pct || mwc->auto_compact_pct ||
             mwc->stall_threshold || mwc->context_window)
         {
            cJSON *mw = cJSON_CreateObject();
            if (mwc->cost_limit)
               cJSON_AddNumberToObject(mw, "cost_limit", mwc->cost_limit);
            if (mwc->context_warn_pct)
               cJSON_AddNumberToObject(mw, "context_warn_pct", mwc->context_warn_pct);
            if (mwc->auto_compact_pct)
               cJSON_AddNumberToObject(mw, "auto_compact_pct", mwc->auto_compact_pct);
            if (mwc->stall_threshold)
               cJSON_AddNumberToObject(mw, "stall_threshold", mwc->stall_threshold);
            if (mwc->context_window)
               cJSON_AddNumberToObject(mw, "context_window", mwc->context_window);
            cJSON_AddItemToObject(a, "middleware", mw);
         }
      }

      cJSON_AddItemToArray(agents, a);
   }
   cJSON_AddItemToObject(root, "agents", agents);

   /* Network config (only write if ssh_entry is set) */
   if (cfg->network.ssh_entry[0])
   {
      const agent_network_t *nw = &cfg->network;
      cJSON *net = cJSON_CreateObject();
      JSON_ADD_STR(net, "ssh_entry", nw->ssh_entry);
      if (nw->ssh_key[0])
         JSON_ADD_STR(net, "ssh_key", nw->ssh_key);

      if (nw->host_count > 0)
      {
         cJSON *hosts = cJSON_CreateArray();
         for (int i = 0; i < nw->host_count; i++)
         {
            const agent_net_host_t *h = &nw->hosts[i];
            cJSON *hobj = cJSON_CreateObject();
            JSON_ADD_STR(hobj, "name", h->name);
            JSON_ADD_STR(hobj, "ip", h->ip);
            if (h->user[0])
               JSON_ADD_STR(hobj, "user", h->user);
            if (h->port > 0)
               cJSON_AddNumberToObject(hobj, "port", h->port);
            if (h->desc[0])
               JSON_ADD_STR(hobj, "desc", h->desc);
            if (h->tunnel[0])
               JSON_ADD_STR(hobj, "tunnel", h->tunnel);
            cJSON_AddItemToArray(hosts, hobj);
         }
         cJSON_AddItemToObject(net, "hosts", hosts);
      }

      if (nw->network_count > 0)
      {
         cJSON *nets = cJSON_CreateArray();
         for (int i = 0; i < nw->network_count; i++)
         {
            const agent_net_def_t *nd = &nw->networks[i];
            cJSON *nobj = cJSON_CreateObject();
            JSON_ADD_STR(nobj, "name", nd->name);
            JSON_ADD_STR(nobj, "cidr", nd->cidr);
            if (nd->desc[0])
               JSON_ADD_STR(nobj, "desc", nd->desc);
            cJSON_AddItemToArray(nets, nobj);
         }
         cJSON_AddItemToObject(net, "networks", nets);
      }

      /* Tunnel config */
      if (cfg->tunnel_mgr.tunnel_count > 0)
      {
         cJSON *tarr = cJSON_CreateArray();
         for (int i = 0; i < cfg->tunnel_mgr.tunnel_count; i++)
         {
            const agent_tunnel_t *tun = &cfg->tunnel_mgr.tunnels[i];
            cJSON *tobj = cJSON_CreateObject();
            JSON_ADD_STR(tobj, "name", tun->name);
            JSON_ADD_STR(tobj, "relay_ssh", tun->relay_ssh);
            if (tun->relay_key[0])
               JSON_ADD_STR(tobj, "relay_key", tun->relay_key);
            JSON_ADD_STR(tobj, "target_host", tun->target_host);
            cJSON_AddNumberToObject(tobj, "target_port", tun->target_port);
            if (tun->reconnect_delay_s != 5)
               cJSON_AddNumberToObject(tobj, "reconnect_delay", tun->reconnect_delay_s);
            if (tun->max_reconnects > 0)
               cJSON_AddNumberToObject(tobj, "max_reconnects", tun->max_reconnects);
            cJSON_AddItemToArray(tarr, tobj);
         }
         cJSON_AddItemToObject(net, "tunnels", tarr);
      }

      cJSON_AddItemToObject(root, "network", net);
   }

   char *json = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json)
      return -1;

   /* Never overwrite a populated agents.json with an empty registry. A zero-agent
    * save is legitimate ONLY on a fresh install (no existing file, or an existing
    * file that already has none) — never as a silent wipe of configured agents.
    * This is the agents.json-deletion guard: whatever the trigger (a load that
    * came back empty, a caller with a zeroed cfg), the destruction stops here, and
    * it stops LOUDLY so the next occurrence is diagnosable rather than invisible. */
   if (cfg->agent_count == 0)
   {
      int existing = agent_config_existing_agent_count();
      if (existing > 0)
      {
         aimee_log(LOG_ERROR, "agent-config",
                   "REFUSING to overwrite %d configured agent(s) in %s with an empty registry. "
                   "An empty save is only valid on a fresh install; this is the deletion guard. "
                   "If a wipe is truly intended, remove the file first.",
                   existing, agent_config_path());
         free(json);
         return -1;
      }
   }

   /* Ensure directory exists */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", config_default_dir());
   platform_mkdir_p(dir, 0700);

   /* Write atomically: a temp file, fsync, then rename over the target. rename(2)
    * is atomic on a POSIX filesystem, so an interrupted or failed write can NEVER
    * leave a truncated or empty agents.json — the previous file survives untouched.
    * The old fopen(path, "w") truncated the real file to zero the instant it
    * opened, before a single byte was written; any hiccup then destroyed it, which
    * on a tiered/networked home is exactly how it went missing. */
   char tmp[MAX_PATH_LEN];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", agent_config_path(), (int)getpid()) >=
       sizeof(tmp))
   {
      free(json);
      return -1;
   }
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      free(json);
      return -1;
   }
   size_t jlen = strlen(json);
   int ok = (write(fd, json, jlen) == (ssize_t)jlen) && (write(fd, "\n", 1) == 1);
   /* fsync before rename: a rename can otherwise be durable while the data it
    * points at is not, leaving an empty file after a crash — the very failure this
    * guards against. */
   if (ok && fsync(fd) != 0)
      ok = 0;
   if (close(fd) != 0)
      ok = 0;
   if (!ok || rename(tmp, agent_config_path()) != 0)
   {
      unlink(tmp); /* leave the existing agents.json intact */
      aimee_log(LOG_ERROR, "agent-config", "atomic write of %s failed; existing file left intact",
                agent_config_path());
      free(json);
      return -1;
   }
   chmod(agent_config_path(), 0600);
   {
      struct stat st;
      if (stat(agent_config_path(), &st) == 0)
      {
         pthread_mutex_lock(&g_agent_config_cache_lock);
         memcpy(&g_agent_config_cache, cfg, sizeof(g_agent_config_cache));
         agent_config_stat_remember(&st);
         g_agent_config_cached = 1;
         pthread_mutex_unlock(&g_agent_config_cache_lock);
      }
   }
   free(json);
   return 0;
}

/* --- Routing --- */

int agent_has_role(const agent_t *agent, const char *role)
{
   for (int i = 0; i < agent->role_count; i++)
   {
      /* "all" is a wildcard: the agent serves every role (routing only — tool
       * use is still governed by exec_roles / tools_enabled). */
      if (strcmp(agent->roles[i], "all") == 0 || strcmp(agent->roles[i], role) == 0)
         return 1;
   }
   return 0;
}

/* 1 if the agent may be dispatched AS `persona`. An agent with no personas list
 * (backward-compatible default) or one containing the "all" wildcard serves any
 * persona; otherwise the persona must be listed. A NULL/empty persona is treated
 * as unconstrained (routing then depends on role alone). */
int agent_supports_persona(const agent_t *agent, const char *persona)
{
   if (!agent)
      return 0;
   if (!persona || !persona[0] || agent->persona_count == 0)
      return 1;
   for (int i = 0; i < agent->persona_count; i++)
   {
      if (strcmp(agent->personas[i], "all") == 0 || strcmp(agent->personas[i], persona) == 0)
         return 1;
   }
   return 0;
}

static int agent_supports_role(const agent_t *agent, const char *role)
{
   if (agent_has_role(agent, role))
      return 1;

   /* Execution roles can be handled by any enabled agent.  The tools_enabled
    * flag controls whether tool use is offered during execution, not whether
    * the agent is eligible for selection. */
   if (agent_is_exec_role(agent, role))
      return 1;

   return 0;
}

static int agent_command_on_path(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;

   char *tokens[32] = {0};
   int count = shlex_split(cmd, tokens, 32);
   if (count <= 0 || count >= 32)
   {
      util_free_tokens(tokens, count);
      return 0;
   }
   for (int i = 0; i < count; i++)
   {
      if (util_token_is_shell_operator(tokens[i]))
      {
         util_free_tokens(tokens, count);
         return 0;
      }
   }
   const char *exe = (count > 0 && tokens[0]) ? tokens[0] : cmd;
   int available = 0;

   if (strchr(exe, '/'))
   {
      available = access(exe, X_OK) == 0;
   }
   else
   {
      const char *path = getenv("PATH");
      if (!path || !path[0])
         path = "/usr/local/bin:/usr/bin:/bin";

      char *copy = safe_strdup(path);
      char *saveptr = NULL;
      for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr))
      {
         char candidate[MAX_PATH_LEN];
         snprintf(candidate, sizeof(candidate), "%s/%s", dir[0] ? dir : ".", exe);
         if (access(candidate, X_OK) == 0)
         {
            available = 1;
            break;
         }
      }
      free(copy);
   }

   util_free_tokens(tokens, count);
   return available;
}

/* Optional route-time health filter; see agent_set_route_health_filter. */
static int (*g_route_health_filter)(const char *agent_name) = NULL;

void agent_set_route_health_filter(int (*fn)(const char *agent_name))
{
   g_route_health_filter = fn;
}

/* Optional route-time delegate-policy filter; see agent_set_route_policy_filter. */
static int (*g_route_policy_filter)(const agent_t *agent) = NULL;

void agent_set_route_policy_filter(int (*fn)(const agent_t *agent))
{
   g_route_policy_filter = fn;
}

/* See agent_config.h: marks the current thread's turn as PRIMARY (not
 * delegation) so the policy filter doesn't exclude the provider-named agent
 * from its own chat turn. */
static _Thread_local int g_routing_primary_turn;

void agent_routing_set_primary_turn(int on)
{
   g_routing_primary_turn = on ? 1 : 0;
}

int agent_routing_primary_turn(void)
{
   return g_routing_primary_turn;
}

int agent_is_available_for_routing(const agent_t *agent)
{
   if (!agent)
      return 0;
   /* A provider the health catalog has marked unavailable (e.g. DOWN after a
    * failure streak) must not receive new routed work, or delegates wedge on
    * a dead endpoint. Treat it like a disabled agent so callers fall back to a
    * healthy peer; routing returns NULL (clean "no agent" error) only when
    * every candidate is filtered out. */
   if (g_route_health_filter && agent->name[0] && g_route_health_filter(agent->name))
      return 0;
   /* A claude-CLI agent can only ever execute as a delegate SERVER-SIDE (a
    * client-only claude has no server session to drive — dispatch would just
    * fail). Structural, so it is enforced even with no policy filter
    * registered; the per-agent rules (the `primary_only` opt-out, primary
    * self-delegation) live in the registered policy filter. */
   if (agent_is_claude_cli(agent) && !agent->is_server_hosted)
      return 0;
   if (g_route_policy_filter && g_route_policy_filter(agent))
      return 0;
   if (strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0)
   {
      const char *cmd =
          agent->cli_cmd[0] ? agent->cli_cmd : (agent->cli_kind[0] ? agent->cli_kind : "claude");
      return agent_command_on_path("tmux") && agent_command_on_path(cmd);
   }

   if (strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) != 0 &&
       strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) != 0)
      return agent_has_resolvable_credentials(agent);

   const provider_cli_adapter_t *adapter = provider_cli_adapter_get(agent->cli_kind);
   if (adapter && adapter->native_provider && adapter->native_provider[0])
      return 1;

   const char *cmd = agent->cli_cmd[0] ? agent->cli_cmd : agent->cli_kind;
   return agent_command_on_path(cmd);
}

int agent_any_delegate_available(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      return 0;
   for (int i = 0; i < cfg.agent_count; i++)
      if (cfg.agents[i].enabled && agent_is_available_for_routing(&cfg.agents[i]))
         return 1;
   return 0;
}

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   }
   return NULL;
}

agent_t *agent_default_primary(agent_config_t *cfg)
{
   /* An explicitly configured default wins — but only when it is actually
    * usable. Routing to a disabled default (or, historically, a disabled
    * agents[0]) makes every ingress request that doesn't name a model fast-fail
    * as "failed to reach the primary provider" even though enabled agents
    * exist, so the fallback deliberately skips disabled seats. */
   if (cfg->default_agent[0])
   {
      agent_t *ag = agent_find(cfg, cfg->default_agent);
      if (ag && ag->enabled)
         return ag;
   }
   for (int i = 0; i < cfg->agent_count; i++)
      if (cfg->agents[i].enabled)
         return &cfg->agents[i];
   return NULL;
}

/* Pick randomly from an array of candidates using monotonic-clock nanoseconds.
 * Thread-safe: no shared mutable state. */
static agent_t *agent_pick_random(agent_t **candidates, int count)
{
   if (count <= 0)
      return NULL;
   if (count == 1)
      return candidates[0];
   struct timespec _ts;
   clock_gettime(CLOCK_MONOTONIC, &_ts);
   unsigned int seed = (unsigned int)_ts.tv_nsec ^ (unsigned int)_ts.tv_sec;
   return candidates[seed % (unsigned int)count];
}

int agent_is_claude_cli(const agent_t *agent)
{
   if (!agent)
      return 0;
   /* Only the Claude CLI (`claude` / `claude-code`) run via tmux or the
    * provider-CLI binary — i.e. authenticated by the interactive `claude` login,
    * NOT an API key. Other CLI agents (Codex CLI, gemini-cli, …) are not gated. */
   if (strcmp(agent->cli_kind, "claude") != 0 && strcmp(agent->cli_kind, "claude-code") != 0)
      return 0;
   return strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0 ||
          strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 ||
          strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) == 0;
}

/* --- generalized role dispatch: a viable delegate for a role ---
 * Return the index of an enabled, routable agent that serves `role` and is not
 * named in `exclude`, chosen uniformly at random among the eligible set (so
 * repeated requests for the same role vary — a roundtable of N `review`
 * delegates, excluding those already used, gets diverse reviewers). Returns -1
 * when none remain. Callers loop: pick -> run -> on failure add the agent to
 * `exclude` -> pick again, until one works. Eligibility + retry-until-viable is
 * the whole mechanism; a specific agent is used only when a caller pins one. */
static unsigned g_role_rand_seed;
static int g_role_rand_seeded;
void delegate_role_pick_seed(unsigned seed)
{
   g_role_rand_seed = seed;
   g_role_rand_seeded = 1;
}
static unsigned delegate_role_rand(void)
{
   if (g_role_rand_seeded)
      return (unsigned)rand_r(&g_role_rand_seed);
   unsigned v = 0;
   FILE *f = fopen("/dev/urandom", "rb");
   if (f)
   {
      if (fread(&v, 1, sizeof v, f) != sizeof v)
         v = 0;
      fclose(f);
   }
   if (!v)
      v = (unsigned)time(NULL);
   return v;
}
int delegate_pick_for_role(agent_config_t *cfg, const char *role, const char *const exclude[],
                           int nexclude)
{
   if (!cfg || !role || !role[0])
      return -1;
   int elig[MAX_AGENTS];
   int n = 0;
   for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      int skip = 0;
      for (int e = 0; e < nexclude && !skip; e++)
         if (exclude[e] && strcmp(exclude[e], ag->name) == 0)
            skip = 1;
      if (skip)
         continue;
      elig[n++] = i;
   }
   if (n == 0)
      return -1;
   return elig[delegate_role_rand() % (unsigned)n];
}

int agent_pick_named_for_role(agent_config_t *cfg, const char *name, const char *role)
{
   if (!cfg || !name || !name[0] || !role || !role[0])
      return -1;
   for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (strcmp(ag->name, name) != 0)
         continue;
      /* Same eligibility triple delegate_pick_for_role applies — a pinned seat
       * resolves with NO substitution, so an agent that exists but is disabled,
       * lacks the role, or is unroutable reports -1 (caller fails the run). */
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         return -1;
      return i;
   }
   return -1;
}

agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   agent_t *def = NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

   /* First pass: find the minimum tier; note if any tmux agent is there
    * (tmux sessions are stateful and always preferred over HTTP peers). */
   int min_tier = -1;
   int has_tmux = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (min_tier < 0 || ag->cost_tier < min_tier)
      {
         min_tier = ag->cost_tier;
         has_tmux = 0;
      }
      if (ag->cost_tier == min_tier && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) == 0)
         has_tmux = 1;
   }
   if (min_tier < 0)
      return NULL;

   /* Second pass: collect candidates at min_tier (tmux-only if any exist). */
   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (ag->cost_tier != min_tier)
         continue;
      if (has_tmux && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) != 0)
         continue;
      if (ag == def)
         return ag;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_random(candidates, count);
}

/* Route to a randomly selected enabled agent at exactly the given cost_tier. */
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier)
{
   agent_t *def = NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || ag->cost_tier != tier || !agent_is_available_for_routing(ag))
         continue;
      if (role && !agent_supports_role(ag, role))
         continue;
      if (ag == def)
         return ag;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_random(candidates, count);
}

static int agent_satisfies_required_caps(const agent_t *ag, unsigned required_caps, int min_context)
{
   if (!ag)
      return 0;

   unsigned missing_caps = required_caps;
   if (ag->tools_enabled)
      missing_caps &= ~MODEL_CAP_TOOLS;

   if (required_caps == 0 && min_context <= 0)
      return 1;

   /* An explicit per-agent context_window override (agents.json
    * `middleware.context_window`, set via `aimee agent --ctx` or auto-detected
    * by `ag_probe_slots`) is authoritative for the min_context gate, so a model
    * the capability catalog doesn't know about is a config change rather than a
    * code change to the registry table. */
   int override_ctx = ag->middleware.context_window;

   model_capability_t caps;
   if (model_capability_get(ag->provider, ag->model, &caps) == 0)
   {
      if (missing_caps != 0)
         return 0;
      /* No catalog entry: the override is the only context signal we have. */
      return min_context <= 0 || (override_ctx > 0 && override_ctx >= min_context);
   }

   if (ag->tools_enabled)
      caps.flags |= MODEL_CAP_TOOLS;
   else
      caps.flags &= ~MODEL_CAP_TOOLS;
   if (required_caps && (caps.flags & required_caps) != required_caps)
      return 0;
   int effective_ctx = override_ctx > 0 ? override_ctx : caps.context_window;
   if (min_context > 0 && effective_ctx > 0 && effective_ctx < min_context)
      return 0;
   if (caps.deprecated)
      return 0;
   return 1;
}

/* Route to the cheapest capable agent, filtering by required capability flags and minimum context
 * window when sys_cfg->model_meta_capability_routing is enabled.  Falls back to plain agent_route
 * when capability routing is disabled. */
static agent_t *agent_route_with_caps_inner(agent_config_t *cfg, const char *role,
                                            const config_t *sys_cfg, unsigned required_caps,
                                            int min_context)
{
   if (!sys_cfg || !sys_cfg->model_meta_capability_routing)
      return agent_route(cfg, role);

   int min_tier = -1;
   int has_tmux = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;

      if (required_caps || min_context > 0)
      {
         if (!agent_satisfies_required_caps(ag, required_caps, min_context))
            goto next_agent;
      }

      if (min_tier < 0 || ag->cost_tier < min_tier)
      {
         min_tier = ag->cost_tier;
         has_tmux = 0;
      }
      if (ag->cost_tier == min_tier && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) == 0)
         has_tmux = 1;
   next_agent:;
   }

   if (min_tier < 0)
      return NULL;

   agent_t *def = NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (ag->cost_tier != min_tier)
         continue;
      if (has_tmux && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) != 0)
         continue;

      if (required_caps || min_context > 0)
      {
         if (!agent_satisfies_required_caps(ag, required_caps, min_context))
            continue;
      }

      if (ag == def)
         return ag;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_random(candidates, count);
}

agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role, const config_t *sys_cfg,
                               unsigned required_caps, int min_context)
{
   agent_t *r = agent_route_with_caps_inner(cfg, role, sys_cfg, required_caps, min_context);
   /* Modality caps (vision/pdf/audio) are inferred from prompt text and are
    * best-effort: if no model satisfies them, relax them and route on the hard
    * caps (tools) + min_context rather than returning no route at all. Mirrors
    * delegate_filter_route_capabilities so both routing gates agree. */
   if (!r && sys_cfg && sys_cfg->model_meta_capability_routing &&
       (required_caps & MODEL_CAP_MODALITY_SOFT))
      r = agent_route_with_caps_inner(cfg, role, sys_cfg, required_caps & ~MODEL_CAP_MODALITY_SOFT,
                                      min_context);
   return r;
}

/* --- Exec role check --- */

static const char *default_exec_roles[] = {
    "deploy", "validate", "test", "diagnose", "execute", "review", "code", "refactor", "draft",
    "implement",
    /* Novel-mode roles: routable + tool-enabled on any default agent. */
    "continuity", "prose", "line-edit", "beat-check",
    /* Songwriter-mode roles. */
    "lyric", "hook", "prosody", "songform"};
#define DEFAULT_EXEC_ROLE_COUNT 18

int agent_is_exec_role(const agent_t *agent, const char *role)
{
   if (agent->exec_role_count > 0)
   {
      for (int i = 0; i < agent->exec_role_count; i++)
      {
         if (strcmp(agent->exec_roles[i], role) == 0)
            return 1;
      }
      return 0;
   }
   for (int i = 0; i < DEFAULT_EXEC_ROLE_COUNT; i++)
   {
      if (strcmp(default_exec_roles[i], role) == 0)
         return 1;
   }
   return 0;
}

/* --- Auth resolution --- */

/* The codex CLI's public OAuth client + token endpoint, as carried verbatim in
 * the codex access token's own `client_id`/`iss` claims (stable public values).
 * Used to auto-refresh the codex bearer through aimee's vault-backed oauth store. */
#define CODEX_OAUTH_STORE          "codex" /* aimee oauth-store client name (vault key) */
#define CODEX_OAUTH_CLIENT_ID      "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_OAUTH_TOKEN_ENDPOINT "https://auth.openai.com/oauth/token"

/* Refresh the codex token this many seconds before its JWT `exp`. Overridable via
 * AIMEE_CODEX_REFRESH_SKEW (set huge to force a refresh — used to live-verify). */
static int codex_oauth_refresh_skew(void)
{
   const char *e = getenv("AIMEE_CODEX_REFRESH_SKEW");
   if (e && e[0])
   {
      long v = atol(e);
      if (v >= 0)
         return (int)v;
   }
   return 3600; /* 1h */
}

/* Decode a JWT's `exp` (Unix seconds); 0 if unparseable. Base64url-decodes the
 * payload (middle) segment and reads the exp number — no signature check (we only
 * need expiry to schedule a refresh). */
static long codex_jwt_exp(const char *jwt)
{
   if (!jwt || !jwt[0])
      return 0;
   const char *p1 = strchr(jwt, '.');
   if (!p1)
      return 0;
   const char *seg = p1 + 1;
   const char *p2 = strchr(seg, '.');
   if (!p2 || p2 == seg)
      return 0;
   size_t seglen = (size_t)(p2 - seg);
   if (seglen > 8192)
      return 0;
   /* base64url -> base64 + pad */
   char b64[8200];
   size_t j = 0;
   for (size_t i = 0; i < seglen && j + 1 < sizeof(b64); i++)
   {
      char c = seg[i];
      b64[j++] = (c == '-') ? '+' : (c == '_') ? '/' : c;
   }
   while ((j % 4) != 0 && j + 1 < sizeof(b64))
      b64[j++] = '=';
   b64[j] = '\0';
   unsigned char dec[8200];
   size_t dn = aimee_base64_decode(b64, dec, sizeof(dec) - 1);
   if (dn == 0)
      return 0;
   dec[dn] = '\0';
   cJSON *root = cJSON_Parse((const char *)dec);
   long exp = 0;
   if (root)
   {
      cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "exp");
      if (cJSON_IsNumber(e))
         exp = (long)e->valuedouble;
      cJSON_Delete(root);
   }
   return exp;
}

/* Read access_token + refresh_token from a codex auth.json (top-level keys or the
 * `tokens.{}` object). Returns 0 only when BOTH are present (a refresh_token is
 * required to manage the token in aimee's store). */
static int codex_read_oauth_pair(const char *path, char *access, size_t an, char *refresh,
                                 size_t rn)
{
   if (access && an)
      access[0] = '\0';
   if (refresh && rn)
      refresh[0] = '\0';
   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      return -1;
   }
   rewind(f);
   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return -1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   fclose(f);
   data[nread] = '\0';
   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      return -1;
   cJSON *at = cJSON_GetObjectItemCaseSensitive(root, "access_token");
   cJSON *rt = cJSON_GetObjectItemCaseSensitive(root, "refresh_token");
   cJSON *tokens = cJSON_GetObjectItemCaseSensitive(root, "tokens");
   if (cJSON_IsObject(tokens))
   {
      if (!cJSON_IsString(at))
         at = cJSON_GetObjectItemCaseSensitive(tokens, "access_token");
      if (!cJSON_IsString(rt))
         rt = cJSON_GetObjectItemCaseSensitive(tokens, "refresh_token");
   }
   int ok = 0;
   if (cJSON_IsString(at) && at->valuestring[0] && cJSON_IsString(rt) && rt->valuestring[0])
   {
      snprintf(access, an, "%s", at->valuestring);
      snprintf(refresh, rn, "%s", rt->valuestring);
      ok = 1;
   }
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

/* Vault-backed, auto-refreshing codex bearer. On first use, bootstrap aimee's
 * oauth store (server-sealed vault) from the codex CLI's auth.json; thereafter
 * oauth_token_get() returns a token, refreshing via the stored refresh_token
 * against OpenAI's token endpoint whenever it is within the skew of expiry —
 * without ever rewriting the CLI's shared auth.json. Returns 0 + a bearer in
 * |buf|; -1 to fall back to the legacy on-disk read. */
static int codex_oauth_vault_token(char *buf, size_t len)
{
   for (int tries = 0; tries < 2; tries++)
   {
      if (oauth_token_get(CODEX_OAUTH_STORE, CODEX_OAUTH_CLIENT_ID, CODEX_OAUTH_TOKEN_ENDPOINT,
                          codex_oauth_refresh_skew(), buf, len) == 0 &&
          buf[0])
         return 0;
      if (tries > 0)
         break; /* bootstrapped already and still no token -> give up */
      /* Bootstrap (or re-bootstrap after a failed refresh) from the on-disk auth. */
      char path[MAX_PATH_LEN], access[MAX_API_KEY_LEN] = "", refresh[1024] = "";
      int got = 0;
      snprintf(path, sizeof(path), "%s/codex-auth.json", config_default_dir());
      if (codex_read_oauth_pair(path, access, sizeof(access), refresh, sizeof(refresh)) == 0)
         got = 1;
      const char *home = getenv("HOME");
      if (!got && home && home[0])
      {
         snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
         if (codex_read_oauth_pair(path, access, sizeof(access), refresh, sizeof(refresh)) == 0)
            got = 1;
      }
      if (!got)
         return -1;
      oauth_token_response_t resp;
      memset(&resp, 0, sizeof(resp));
      snprintf(resp.access_token, sizeof(resp.access_token), "%s", access);
      snprintf(resp.refresh_token, sizeof(resp.refresh_token), "%s", refresh);
      resp.expires_at = codex_jwt_exp(access); /* 0 = unknown (no proactive refresh) */
      if (oauth_token_store(CODEX_OAUTH_STORE, &resp) != 0)
         return -1;
      /* loop: oauth_token_get now serves the vaulted token (refreshing if stale). */
   }
   return -1;
}

static int agent_read_codex_oauth_token_from_path(const char *path, char *token, size_t token_len)
{
   if (!path || !path[0] || !token || token_len == 0)
      return -1;
   token[0] = '\0';

   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return -1;
   }
   long sz = ftell(f);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      return -1;
   }
   rewind(f);

   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return -1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   fclose(f);
   data[nread] = '\0';

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      return -1;

   cJSON *access = cJSON_GetObjectItem(root, "access_token");
   if (!access)
   {
      cJSON *tokens = cJSON_GetObjectItem(root, "tokens");
      if (tokens && cJSON_IsObject(tokens))
         access = cJSON_GetObjectItem(tokens, "access_token");
   }
   if (access && cJSON_IsString(access) && access->valuestring[0])
      snprintf(token, token_len, "%s", access->valuestring);
   cJSON_Delete(root);

   return token[0] ? 0 : -1;
}

static int agent_read_codex_oauth_token(char *token, size_t token_len)
{
   if (!token || token_len == 0)
      return -1;
   token[0] = '\0';

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/codex-auth.json", config_default_dir());
   if (agent_read_codex_oauth_token_from_path(path, token, token_len) == 0)
      return 0;

   const char *home = getenv("HOME");
   if (home && home[0])
   {
      snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
      if (agent_read_codex_oauth_token_from_path(path, token, token_len) == 0)
         return 0;
   }

   return -1;
}

int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len)
{
   buf[0] = '\0';
   g_request_auth_error[0] = '\0'; /* reset the per-turn explicit-error channel (D6) */
   const char *auth_type = agent->auth_type;
   if (strcmp(agent->provider, "anthropic") == 0 &&
       (!auth_type[0] || strcmp(auth_type, "bearer") == 0 || strcmp(auth_type, "api_key") == 0))
      auth_type = "x-api-key";

   if (strcmp(auth_type, "none") == 0)
      return 0;

   if (strcmp(auth_type, "codex-oauth") == 0)
   {
      /* Codex OAuth token, in precedence: per-turn token (set by the vault delegate
       * path) > the permanent VAULT (turn principal, then server principal) >
       * server-side file (legacy on-disk codex auth). */
      if (g_request_codex_token[0])
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", g_request_codex_token);
         return 0;
      }
      char token[MAX_API_KEY_LEN];
      if (agent_vault_get(agent->name, VAULT_CODEX_TOKEN_CRED, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
      /* Vault-backed, auto-refreshing token (bootstrapped from the codex auth.json):
       * preferred over the raw on-disk read so an expired access token self-heals
       * via the stored refresh_token instead of 401ing. */
      if (codex_oauth_vault_token(token, sizeof(token)) == 0)
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
      if (agent_read_codex_oauth_token(token, sizeof(token)) != 0)
      {
         /* No usable codex token. If a prior refresh was rejected by the IdP, the
          * refresh token is dead and the server cannot recover on its own — give
          * the operator the exact remedy instead of a generic provider 401 (D6). */
         if (oauth_token_reauth_required(CODEX_OAUTH_STORE))
            snprintf(g_request_auth_error, sizeof(g_request_auth_error),
                     "codex re-auth required: the stored OAuth refresh token was rejected — run "
                     "`aimee codex reauth` to re-authenticate");
         return -1;
      }
      snprintf(buf, buf_len, "Authorization: Bearer %s", token);
      return 0;
   }

   if (strcmp(auth_type, "oauth") == 0 && agent->auth_cmd[0])
   {
      /* Run auth_cmd via safe_exec_capture (no shell injection) */
      char *auth_tokens[32];
      int auth_tc = shlex_split(agent->auth_cmd, auth_tokens, 32);
      if (auth_tc <= 0)
         return -1;
      const char *auth_argv[33];
      for (int ai = 0; ai < auth_tc && ai < 32; ai++)
         auth_argv[ai] = auth_tokens[ai];
      auth_argv[auth_tc] = NULL;
      char *output = NULL;
      int status = safe_exec_capture(auth_argv, &output, MAX_API_KEY_LEN);
      for (int ai = 0; ai < auth_tc; ai++)
         free(auth_tokens[ai]);
      if (status != 0 || !output || !output[0])
      {
         free(output);
         return -1;
      }
      char token[MAX_API_KEY_LEN];
      snprintf(token, sizeof(token), "%s", output);
      free(output);
      /* Strip trailing newline */
      size_t len = strlen(token);
      while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
         token[--len] = '\0';
      if (!token[0])
         return -1;

      snprintf(buf, buf_len, "Authorization: Bearer %s", token);
      return 0;
   }

   /* Credential precedence for the in-flight turn (P4): the permanent VAULT wins
    * — resolved for the turn's attested principal with autonomous server-principal
    * fallback, so EVERY connection (primary chat, webchat, in-model tool, delegate)
    * is served from the one store; then the agent's stored api_key / env below.
    * vault_service_inject_api_key handles the dual-wrap (server-wrap -> user-KEK ->
    * server principal) AND is robust to an empty principal (it just resolves the
    * server principal); a locked/miss result falls through here, while the delegate
    * path keeps its own D15 hard-fail-on-locked. Computed only for the key-bearing
    * auth types below (not codex-oauth / auth_cmd), to avoid a wasted decrypt on
    * turns that never consult it. */
   char primary_key[MAX_API_KEY_LEN];
   int have_primary_key =
       (vault_service_inject_api_key(agent_get_request_vault_principal(), agent->name, primary_key,
                                     sizeof(primary_key), time(NULL)) == VAULT_OK &&
        primary_key[0]);

   /* x-api-key auth (Anthropic): resolve via the vault, auth_cmd or api_key */
   if (strcmp(auth_type, "x-api-key") == 0)
   {
      if (have_primary_key)
      {
         snprintf(buf, buf_len, "x-api-key: %s", primary_key);
         return 0;
      }
      if (agent->auth_cmd[0])
      {
         char *auth_tokens[32];
         int auth_tc = shlex_split(agent->auth_cmd, auth_tokens, 32);
         if (auth_tc <= 0)
            return -1;
         const char *auth_argv[33];
         for (int ai = 0; ai < auth_tc && ai < 32; ai++)
            auth_argv[ai] = auth_tokens[ai];
         auth_argv[auth_tc] = NULL;
         char *output = NULL;
         int status = safe_exec_capture(auth_argv, &output, MAX_API_KEY_LEN);
         for (int ai = 0; ai < auth_tc; ai++)
            free(auth_tokens[ai]);
         if (status != 0 || !output || !output[0])
         {
            free(output);
            return -1;
         }
         char token[MAX_API_KEY_LEN];
         snprintf(token, sizeof(token), "%s", output);
         free(output);
         size_t len = strlen(token);
         while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
            token[--len] = '\0';
         if (!token[0])
            return -1;
         snprintf(buf, buf_len, "x-api-key: %s", token);
         return 0;
      }
      if (agent_api_key_literal(agent->api_key))
      {
         snprintf(buf, buf_len, "x-api-key: %s", agent->api_key);
         return 0;
      }
      char token[MAX_API_KEY_LEN];
      if (agent_provider_env_value(agent->provider, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "x-api-key: %s", token);
         return 0;
      }
   }

   if (strcmp(agent->provider, "gemini") == 0 &&
       (!auth_type[0] || strcmp(auth_type, "api_key") == 0))
   {
      if (have_primary_key)
      {
         snprintf(buf, buf_len, "x-goog-api-key: %s", primary_key);
         return 0;
      }
      if (agent_api_key_literal(agent->api_key))
      {
         snprintf(buf, buf_len, "x-goog-api-key: %s", agent->api_key);
         return 0;
      }
      char token[MAX_API_KEY_LEN];
      if (agent_provider_env_value(agent->provider, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "x-goog-api-key: %s", token);
         return 0;
      }
   }

   /* Default: bearer token — session keyring first, then stored api_key. */
   if (have_primary_key)
   {
      snprintf(buf, buf_len, "Authorization: Bearer %s", primary_key);
      return 0;
   }
   if (agent_api_key_literal(agent->api_key))
   {
      snprintf(buf, buf_len, "Authorization: Bearer %s", agent->api_key);
      return 0;
   }
   {
      char token[MAX_API_KEY_LEN];
      if (agent_provider_env_value(agent->provider, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
   }

   return 0; /* no auth needed (e.g., local Ollama) */
}
