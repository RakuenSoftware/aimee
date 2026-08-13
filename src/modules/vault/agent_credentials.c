/* agent_credentials.c: resolving an agent's credentials, and the per-request
 * identity that resolution runs under.
 *
 * SPLIT OUT OF agent_config.c, WHICH IS WHY THAT FILE COULD NOT MOVE. agents.json
 * is configuration and belongs in the config module, but the loader also reached
 * straight into the vault module (runtime_secret.h, vault_service.h,
 * vault_principal.h). Inside a module that is an undeclared peer crossing --
 * module-bus-boundary-check rejects it -- and it was only ever legal because the
 * file sat in src/server/. The name said config; a third of the file was
 * credential resolution and thread-local request state.
 *
 * Nothing here is configuration. It answers "can this agent authenticate right
 * now?" from the vault, the environment and the live request -- all of which
 * change without agents.json changing. */
#include "agent_config.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <strings.h> /* strcasecmp: vault names carry operator casing */

#include "runtime_secret.h"
#include "vault_principal.h"
#include "vault_store.h" /* vault_store_entry_t */

/* A deployment with more than this many vaulted credentials would truncate the
 * listing. It is a lookup bound, not a store bound: exceeding it can only make
 * vault_provider_has_credential answer 0 for a provider filed past the cut, so
 * the failure is "reports unconfigured", never a false [key set]. */
#define VAULT_PROVIDER_LIST_MAX 256
#include "aimee.h"
#include <openssl/crypto.h>
#include "cJSON.h"
#include "oauth_flow.h"    /* vault-backed auto-refreshing codex token */
#include "vault_service.h" /* vault_service_* : the permanent credential store (P4) */

/* --- First-boot environment references ---------------------------------- */

void agent_expand_env(const char *src, char *dst, size_t dst_len)
{
   if (!src || !src[0])
   {
      dst[0] = '\0';
      return;
   }

   if (src[0] == '$')
   {
      /* Credential environment variables are consumed and unset before config
       * loading. The only runtime view is the locked-memory cache hydrated from
       * Vault; dotenv and direct getenv fallbacks are intentionally forbidden. */
      if (runtime_secret_get(src + 1, dst, dst_len))
         return;
      dst[0] = '\0';
      return;
   }

   snprintf(dst, dst_len, "%s", src);
}

/* Which environment variables carry each provider's key. */
static const char *const openrouter_env_vars[] = {"OPENROUTER_API_KEY", NULL};
static const char *const mistral_env_vars[] = {"MISTRAL_API_KEY", NULL};
static const char *const anthropic_env_vars[] = {"ANTHROPIC_API_KEY", NULL};
static const char *const gemini_env_vars[] = {"GEMINI_API_KEY", "GOOGLE_API_KEY", NULL};
static const char *const minimax_env_vars[] = {"MINIMAX_API_KEY", NULL};

static _Thread_local char g_request_session_id[128];

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

static int agent_provider_requires_credentials(const char *provider)
{
   return agent_provider_env_vars(provider) != NULL;
}

static int agent_api_key_literal(const char *api_key)
{
   return api_key && api_key[0] && api_key[0] != '$';
}

/* See agent_config.h. The config module hands us whatever agents.json holds --
 * a literal key or a "$VAR" reference -- and never resolves it, so the resolution
 * lives here and the registry never carries a credential. */
int agent_api_key_secret(const agent_t *agent, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!agent || !agent->api_key[0])
      return 0;
   if (agent->api_key[0] == '$')
   {
      /* Credential environment variables are consumed and unset before config
       * loading; the only runtime view is the locked-memory cache hydrated from
       * Vault. dotenv and direct getenv fallbacks stay forbidden. */
      agent_expand_env(agent->api_key, out, out_len);
      return out[0] ? 1 : 0;
   }
   snprintf(out, out_len, "%s", agent->api_key);
   return 1;
}

static int agent_provider_env_value(const char *provider, char *dst, size_t dst_len)
{
   const char *const *envs = agent_provider_env_vars(provider);
   if (!envs)
      return 0;
   for (int i = 0; envs[i]; i++)
   {
      char value[MAX_API_KEY_LEN];
      if (runtime_secret_get(envs[i], value, sizeof(value)) && value[0])
      {
         if (dst && dst_len > 0)
            snprintf(dst, dst_len, "%s", value);
         runtime_secret_wipe(value, sizeof(value));
         return 1;
      }
   }
   return 0;
}

static int agent_vault_get(const char *agent_name, const char *cred, char *out, size_t out_len);

/* Contract in agent_config.h. Enumerating rather than probing a fixed cred name
 * is deliberate: providers are authenticated several ways (api_key, oauth,
 * codex's token trio) and the question here is only whether the operator put
 * anything under this name, not which credential a later call will use. */
int vault_provider_has_credential(const char *provider_name)
{
   if (!provider_name || !provider_name[0])
      return 0;

   vault_store_entry_t entries[VAULT_PROVIDER_LIST_MAX];
   int count = 0;
   if (vault_service_list(VAULT_SERVER_PRINCIPAL, entries, VAULT_PROVIDER_LIST_MAX, &count) !=
       VAULT_OK)
      return 0;

   for (int i = 0; i < count; i++)
      if (entries[i].cred[0] && strcasecmp(entries[i].agent, provider_name) == 0)
         return 1;
   return 0;
}

int agent_has_resolvable_credentials(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (!agent_provider_requires_credentials(agent->provider))
      return 1;
   if (agent->auth_cmd[0])
      return 1;
   {
      /* A "$VAR" reference counts only if it actually resolves; an unresolved one
       * carries no credential and must fall through to the vault probe below. */
      char probe[MAX_API_KEY_LEN];
      int have = agent_api_key_secret(agent, probe, sizeof(probe));
      runtime_secret_wipe(probe, sizeof(probe));
      if (have)
         return 1;
   }
   /* A credential in the permanent vault (this turn's principal or the server
    * principal) makes the agent usable with no on-disk key — the P4 primary path.
    * For codex we probe the TOKEN, not the account id: a vaulted account without a
    * token is NOT a usable credential (the token is what authenticates). */
   {
      /* PRESENCE, not access. This used to call agent_vault_get, which decrypts
       * the credential into a stack buffer (never wiped here) and emits a
       * vault.get_server audit row -- for a question that only needs to know
       * whether an entry exists. */
      int is_codex = strcmp(agent->auth_type, "codex-oauth") == 0;
      if (vault_service_has_server_principal(agent->name, is_codex ? VAULT_CODEX_TOKEN_CRED
                                                                   : VAULT_API_KEY_CRED))
         return 1;
   }
   for (int i = 0; i < agent->credential_count; i++)
   {
      char value[MAX_API_KEY_LEN];
      if (runtime_secret_get(agent->credentials[i].api_key_env, value, sizeof(value)) && value[0])
      {
         runtime_secret_wipe(value, sizeof(value));
         return 1;
      }
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

/* Read a credential for the one server environment. The in-flight principal is
 * actor attribution only and must never select a model/provider credential. */
static int agent_vault_get(const char *agent_name, const char *cred, char *out, size_t out_len)
{
   if (!agent_name || !agent_name[0] || !out || out_len == 0)
      return 0;
   out[0] = '\0';
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

/* Parse access_token + refresh_token from codex auth JSON (top-level keys or
 * `tokens.{}`). Returns 0 only when both are present: a refresh token is
 * required to manage the credential in AIMEE's Vault-backed OAuth store. */
static int codex_parse_oauth_pair(const char *data, char *access, size_t an, char *refresh,
                                  size_t rn)
{
   if (access && an)
      access[0] = '\0';
   if (refresh && rn)
      refresh[0] = '\0';
   cJSON *root = cJSON_Parse(data);
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
 * oauth store from the server-sealed, opaque codex OAuth document; thereafter
 * oauth_token_get() returns a token, refreshing via the stored refresh_token
 * against OpenAI's token endpoint whenever it is within the skew of expiry —
 * without reading or rewriting a persistent auth.json. Returns 0 + a bearer in
 * |buf|; -1 when Vault has no usable OAuth document. */
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
      /* Bootstrap (or re-bootstrap after a rejected refresh) from the opaque
       * vendor document stored in Vault by the login/legacy-migration path. */
      char access[MAX_API_KEY_LEN] = "", refresh[1024] = "", document[64 * 1024];
      vault_status_t vst =
          vault_service_get_server_principal("codex", "oauth", document, sizeof(document));
      int got = vst == VAULT_OK && codex_parse_oauth_pair(document, access, sizeof(access), refresh,
                                                          sizeof(refresh)) == 0;
      OPENSSL_cleanse(document, sizeof(document));
      if (!got)
         return -1;
      oauth_token_response_t resp;
      memset(&resp, 0, sizeof(resp));
      snprintf(resp.access_token, sizeof(resp.access_token), "%s", access);
      snprintf(resp.refresh_token, sizeof(resp.refresh_token), "%s", refresh);
      resp.expires_at = codex_jwt_exp(access); /* 0 = unknown (no proactive refresh) */
      if (oauth_token_store(CODEX_OAUTH_STORE, &resp) != 0)
      {
         OPENSSL_cleanse(access, sizeof(access));
         OPENSSL_cleanse(refresh, sizeof(refresh));
         return -1;
      }
      OPENSSL_cleanse(access, sizeof(access));
      OPENSSL_cleanse(refresh, sizeof(refresh));
      /* loop: oauth_token_get now serves the vaulted token (refreshing if stale). */
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
       * path) > the permanent Vault (turn principal, then server principal). */
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
      /* Vault-backed, auto-refreshing token bootstrapped from the opaque codex
       * OAuth document already ingested by login or boot migration. */
      if (codex_oauth_vault_token(token, sizeof(token)) == 0)
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
      /* No usable codex token. If a prior refresh was rejected by the IdP, the
       * refresh token is dead and the server cannot recover on its own — give
       * the operator the exact remedy instead of a generic provider 401 (D6). */
      if (oauth_token_reauth_required(CODEX_OAUTH_STORE))
         snprintf(g_request_auth_error, sizeof(g_request_auth_error),
                  "codex re-auth required: the stored OAuth refresh token was rejected — run "
                  "`aimee codex reauth` to re-authenticate");
      return -1;
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
      {
         char key[MAX_API_KEY_LEN];
         if (agent_api_key_secret(agent, key, sizeof(key)))
         {
            snprintf(buf, buf_len, "x-api-key: %s", key);
            runtime_secret_wipe(key, sizeof(key));
            return 0;
         }
         runtime_secret_wipe(key, sizeof(key));
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
