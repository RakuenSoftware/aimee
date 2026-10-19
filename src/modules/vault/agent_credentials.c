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

#include <limits.h>
#include <strings.h> /* strcasecmp: vault names carry operator casing */

#include "runtime_secret.h"
#include "config.h"
#include "platform_path.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
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
#include "aimee_sha256.h"
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
                                                                   : VAULT_API_KEY_CRED) ||
          (agent->registration[0] &&
           vault_service_has_server_principal(agent->registration, is_codex ? VAULT_CODEX_TOKEN_CRED
                                                                            : VAULT_API_KEY_CRED)))
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

/* Per-request host-resolved credentials (see agent_config.h). Thread-local:
 * each chat/delegate turn runs on its own worker thread. */
static _Thread_local char g_request_codex_token[MAX_API_KEY_LEN];
static _Thread_local char g_request_codex_account_id[128];
static _Thread_local char g_request_codex_registration[128];
static _Thread_local char g_request_claude_token[MAX_API_KEY_LEN];
static _Thread_local char g_request_claude_registration[128];
static _Thread_local char g_request_claude_target[65];
static _Thread_local char g_request_api_key[MAX_API_KEY_LEN];
static _Thread_local char g_request_api_registration[128];
static _Thread_local char g_request_api_target[65];

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

const char *agent_get_request_session(void)
{
   return g_request_session_id;
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

/* The server preset is the legacy unnamed context. Named registrations never
 * inherit that account merely because their own credential is missing. */
static const char *codex_registration(const agent_t *agent)
{
   if (agent && agent->registration[0])
      return agent->registration;
   if (agent && agent->name[0])
      return agent->name;
   return "codex";
}

static void set_codex_creds(const char *registration, const char *token, const char *account_id)
{
   OPENSSL_cleanse(g_request_codex_token, sizeof(g_request_codex_token));
   OPENSSL_cleanse(g_request_codex_account_id, sizeof(g_request_codex_account_id));
   g_request_codex_registration[0] = 0;
   if (!token || !token[0])
      return;
   snprintf(g_request_codex_token, sizeof(g_request_codex_token), "%s", token);
   snprintf(g_request_codex_account_id, sizeof(g_request_codex_account_id), "%s",
            account_id ? account_id : "");
   snprintf(g_request_codex_registration, sizeof(g_request_codex_registration), "%s",
            registration && registration[0] ? registration : "codex");
}

void agent_set_request_codex_creds_for(const agent_t *agent, const char *token,
                                       const char *account_id)
{
   set_codex_creds(codex_registration(agent), token, account_id);
}

void agent_set_request_codex_creds(const char *token, const char *account_id)
{
   set_codex_creds("codex", token, account_id);
}

int agent_request_codex_token_matches(const agent_t *agent)
{
   return g_request_codex_token[0] &&
          !strcmp(g_request_codex_registration, codex_registration(agent));
}

void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   if (!out)
      return;
   snprintf(out->session_id, sizeof(out->session_id), "%s", g_request_session_id);
   snprintf(out->codex_token, sizeof(out->codex_token), "%s", g_request_codex_token);
   snprintf(out->codex_account_id, sizeof(out->codex_account_id), "%s", g_request_codex_account_id);
   snprintf(out->codex_registration, sizeof(out->codex_registration), "%s",
            g_request_codex_registration);
   snprintf(out->claude_token, sizeof out->claude_token, "%s", g_request_claude_token);
   snprintf(out->claude_registration, sizeof out->claude_registration, "%s",
            g_request_claude_registration);
   snprintf(out->claude_target, sizeof out->claude_target, "%s", g_request_claude_target);
   snprintf(out->api_key, sizeof out->api_key, "%s", g_request_api_key);
   snprintf(out->api_key_registration, sizeof out->api_key_registration, "%s",
            g_request_api_registration);
   snprintf(out->api_key_target, sizeof out->api_key_target, "%s", g_request_api_target);
   snprintf(out->vault_principal, sizeof(out->vault_principal), "%s", g_request_vault_principal);
}

void agent_request_creds_restore(const agent_request_creds_t *creds)
{
   if (!creds)
      return;
   agent_set_request_session(creds->session_id);
   set_codex_creds(creds->codex_registration, creds->codex_token, creds->codex_account_id);
   OPENSSL_cleanse(g_request_claude_token, sizeof g_request_claude_token);
   snprintf(g_request_claude_token, sizeof g_request_claude_token, "%s", creds->claude_token);
   snprintf(g_request_claude_registration, sizeof g_request_claude_registration, "%s",
            creds->claude_registration);
   snprintf(g_request_claude_target, sizeof g_request_claude_target, "%s", creds->claude_target);
   OPENSSL_cleanse(g_request_api_key, sizeof g_request_api_key);
   snprintf(g_request_api_key, sizeof g_request_api_key, "%s", creds->api_key);
   snprintf(g_request_api_registration, sizeof g_request_api_registration, "%s",
            creds->api_key_registration);
   snprintf(g_request_api_target, sizeof g_request_api_target, "%s", creds->api_key_target);
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

static void build_base_extra_headers(const agent_t *agent, char *buf, size_t buf_len)
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

#define CODEX_SOURCE_CREDENTIAL OAUTH_SOURCE_CREDENTIAL

static void credential_json_cleanse(cJSON *json)
{
   if (!json)
      return;
   for (cJSON *child = json->child; child; child = child->next)
      credential_json_cleanse(child);
   if (cJSON_IsString(json) && json->valuestring)
      OPENSSL_cleanse(json->valuestring, strlen(json->valuestring));
}
static int credential_text(const cJSON *object, const char *name, char *out, size_t capacity,
                           int required)
{
   const cJSON *found = NULL;
   for (const cJSON *item = object ? object->child : NULL; item; item = item->next)
      if (item->string && !strcmp(item->string, name))
      {
         if (found)
            return -1;
         found = item;
      }
   if (!found)
      return required ? -1 : 0;
   if (!cJSON_IsString(found) || strlen(found->valuestring) >= capacity ||
       (required && !found->valuestring[0]))
      return -1;
   snprintf(out, capacity, "%s", found->valuestring);
   return 0;
}
static int credential_source_digest(const char *token, const char *refresh, const char *account,
                                    char out[65])
{
   char material[MAX_API_KEY_LEN + 1024 + 128 + 3];
   const char *fields[] = {token, refresh, account};
   size_t used = 0;
   for (size_t i = 0; i < 3; i++)
   {
      size_t length = strlen(fields[i]) + 1;
      if (length > sizeof material - used)
         return -1;
      memcpy(material + used, fields[i], length);
      used += length;
   }
   int rc = aimee_sha256_hex(material, used, out);
   OPENSSL_cleanse(material, sizeof material);
   return rc;
}
static int codex_parse_source(const char *document, oauth_token_response_t *tokens,
                              char account[128], char digest[65])
{
   /* Native login documents may carry unrelated metadata. Credential fields
    * themselves must be bounded and unambiguous, without NUL/header injection. */
   if (strstr(document, "\\u0000"))
      return -1;
   cJSON *root = cJSON_ParseWithOpts(document, NULL, 1);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return -1;
   }
   const cJSON *source = root;
   const cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "tokens");
   int rc = -1;
   int containers = 0;
   for (const cJSON *field = root->child; field; field = field->next)
      if (field->string && !strcmp(field->string, "tokens") && ++containers > 1)
         goto done;
   if (nested)
   {
      if (!cJSON_IsObject(nested) || cJSON_GetObjectItemCaseSensitive(root, "access_token") ||
          cJSON_GetObjectItemCaseSensitive(root, "refresh_token") ||
          cJSON_GetObjectItemCaseSensitive(root, "account_id"))
         goto done;
      source = nested;
   }
   if (credential_text(source, "access_token", tokens->access_token, sizeof tokens->access_token,
                       1) ||
       credential_text(source, "refresh_token", tokens->refresh_token, sizeof tokens->refresh_token,
                       1) ||
       credential_text(source, "account_id", account, 128, 0) ||
       strpbrk(tokens->access_token, "\r\n") || strpbrk(account, "\r\n"))
      goto done;
   tokens->expires_at = codex_jwt_exp(tokens->access_token);
   rc = credential_source_digest(tokens->access_token, tokens->refresh_token, account, digest);
done:
   credential_json_cleanse(root);
   cJSON_Delete(root);
   return rc;
}
static int codex_credentials_locked(const char *registration, agent_codex_credentials_t *out)
{
   char manual[MAX_API_KEY_LEN], account[128], document[64 * 1024 + 1], marker[65];
   vault_store_read_t fields[] = {{VAULT_CODEX_TOKEN_CRED, manual, sizeof manual, 0},
                                  {VAULT_CODEX_ACCOUNT_CRED, account, sizeof account, 0},
                                  {"oauth", document, sizeof document, 0},
                                  {CODEX_SOURCE_CREDENTIAL, marker, sizeof marker, 0}};
   int rc = -1;
   if (vault_service_read_server_snapshot(registration, fields, 4) != VAULT_OK)
      goto done;
   if (fields[0].present && manual[0])
   {
      if (strpbrk(manual, "\r\n") || strpbrk(account, "\r\n"))
         goto done;
      snprintf(out->token, sizeof out->token, "%s", manual);
      snprintf(out->account_id, sizeof out->account_id, "%s", account);
      rc = credential_source_digest(manual, "", account, out->source_digest);
      goto done;
   }
   if (fields[2].present && document[0])
   {
      oauth_token_response_t source = {0};
      char source_account[128] = "", digest[65] = "";
      if (codex_parse_source(document, &source, source_account, digest))
      {
         OPENSSL_cleanse(&source, sizeof source);
         goto done;
      }
      if (!fields[3].present || strcmp(marker, digest))
      {
         /* Reset unknown/prior-account expiry and rotating credentials only for
          * a genuinely new source. Never overwrite a refreshed token from the
          * unchanged original document after a transient/rejected refresh. */
         if (oauth_token_store_source(registration, &source, digest))
         {
            OPENSSL_cleanse(&source, sizeof source);
            goto done;
         }
      }
      OPENSSL_cleanse(&source, sizeof source);
      if (oauth_token_get(registration, CODEX_OAUTH_CLIENT_ID, CODEX_OAUTH_TOKEN_ENDPOINT,
                          codex_oauth_refresh_skew(), out->token, sizeof out->token) ||
          !out->token[0])
         goto done;
      snprintf(out->account_id, sizeof out->account_id, "%s", source_account);
      snprintf(out->source_digest, sizeof out->source_digest, "%s", digest);
      rc = 0;
      goto done;
   }
   /* A removed, previously bound native source is revoked. The pre-cutover
    * normalized server credential remains a compatible legacy source only when
    * it has never claimed a native-document lineage. Named connections cannot
    * borrow that default account. */
   if (!fields[3].present && !strcmp(registration, CODEX_OAUTH_STORE) &&
       !oauth_token_get(registration, CODEX_OAUTH_CLIENT_ID, CODEX_OAUTH_TOKEN_ENDPOINT,
                        codex_oauth_refresh_skew(), out->token, sizeof out->token) &&
       out->token[0])
   {
      snprintf(out->account_id, sizeof out->account_id, "%s", account);
      rc = credential_source_digest(out->token, "", account, out->source_digest);
   }
done:
   OPENSSL_cleanse(manual, sizeof manual);
   OPENSSL_cleanse(account, sizeof account);
   OPENSSL_cleanse(document, sizeof document);
   OPENSSL_cleanse(marker, sizeof marker);
   return rc;
}
/* Both vendor adapters share the normalized token slots and source fence for
 * a registration. They must also share its refresh lock across family changes. */
static int oauth_connection_lock(const char *registration)
{
   if (!registration || !registration[0] || strlen(registration) >= 128)
      return -1;
   char dir[1024], path[1280], suffix[65];
   const char *home = config_default_dir();
   if (!home || !home[0] || (size_t)snprintf(dir, sizeof dir, "%s/.vault", home) >= sizeof dir ||
       platform_mkdir_p(dir, 0700) || aimee_sha256_hex(registration, strlen(registration), suffix))
      return -1;
   int length = !strcmp(registration, CODEX_OAUTH_STORE)
                    ? snprintf(path, sizeof path, "%s/.codex-oauth.lock", dir)
                    : snprintf(path, sizeof path, "%s/.codex-oauth-%s.lock", dir, suffix);
   if (length < 0 || (size_t)length >= sizeof path)
      return -1;
   int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
       (st.st_mode & 0077) || st.st_nlink != 1 || flock(fd, LOCK_EX))
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   return fd;
}
int agent_codex_connection_credentials(const char *registration, agent_codex_credentials_t *out)
{
   if (!out)
      return -1;
   OPENSSL_cleanse(out, sizeof *out);
   int fd = oauth_connection_lock(registration);
   if (fd < 0)
      return -1;
   int rc = codex_credentials_locked(registration, out);
   close(fd);
   if (rc)
      OPENSSL_cleanse(out, sizeof *out);
   return rc;
}
/* Public subscription OAuth metadata from the installed Claude SDK. The
 * document's persisted clientId wins for newer/custom clients; old logins use
 * the SDK's legacy public client. No endpoint is taken from a login document. */
#define CLAUDE_OAUTH_CLIENT_ID      "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define CLAUDE_OAUTH_TOKEN_ENDPOINT "https://platform.claude.com/v1/oauth/token"

static const cJSON *credential_unique(const cJSON *object, const char *name)
{
   const cJSON *found = NULL;
   for (const cJSON *item = object ? object->child : NULL; item; item = item->next)
      if (item->string && !strcmp(item->string, name))
      {
         if (found)
            return NULL;
         found = item;
      }
   return found;
}

static int claude_parse_source(const char *document, oauth_token_response_t *tokens,
                               char client[256], char scopes[4096], char digest[65])
{
   if (strstr(document, "\\u0000"))
      return -1;
   cJSON *root = cJSON_ParseWithOpts(document, NULL, 1);
   const cJSON *source = credential_unique(root, "claudeAiOauth");
   const cJSON *expiry = credential_unique(source, "expiresAt");
   const cJSON *grants = credential_unique(source, "scopes");
   int rc = -1;
   if (!cJSON_IsObject(root) || !cJSON_IsObject(source) || !cJSON_IsNumber(expiry) ||
       !(expiry->valuedouble > 0 && expiry->valuedouble <= 9007199254740991.0) ||
       expiry->valuedouble / 1000 > (double)LONG_MAX ||
       (double)(long long)expiry->valuedouble != expiry->valuedouble || !cJSON_IsArray(grants) ||
       !grants->child ||
       credential_text(source, "accessToken", tokens->access_token, sizeof tokens->access_token,
                       1) ||
       credential_text(source, "refreshToken", tokens->refresh_token, sizeof tokens->refresh_token,
                       1) ||
       credential_text(source, "clientId", client, 256, 0) ||
       strpbrk(tokens->access_token, "\r\n") || strpbrk(client, "\r\n"))
      goto done;
   if (!client[0])
      snprintf(client, 256, "%s", CLAUDE_OAUTH_CLIENT_ID);
   size_t used = 0;
   for (const cJSON *grant = grants->child; grant; grant = grant->next)
   {
      if (!cJSON_IsString(grant) || !grant->valuestring[0])
         goto done;
      for (const unsigned char *p = (const unsigned char *)grant->valuestring; *p; p++)
         if (*p < 0x21 || *p > 0x7e || *p == '"' || *p == '\\')
            goto done;
      int n = snprintf(scopes + used, 4096 - used, "%s%s", used ? " " : "", grant->valuestring);
      if (n < 0 || (size_t)n >= 4096 - used)
         goto done;
      used += (size_t)n;
   }
   tokens->expires_at = (long)(expiry->valuedouble / 1000);
   if (!tokens->expires_at)
      goto done;
   /* Hash only effective source fields; cosmetic/native metadata cannot reset
    * a rotating refresh token. Domain separation fences Codex/Claude switches. */
   cJSON *identity = cJSON_CreateArray();
   if (identity && cJSON_AddItemToArray(identity, cJSON_CreateString("claude-oauth-v1")) &&
       cJSON_AddItemToArray(identity, cJSON_CreateString(tokens->access_token)) &&
       cJSON_AddItemToArray(identity, cJSON_CreateString(tokens->refresh_token)) &&
       cJSON_AddItemToArray(identity, cJSON_CreateString(client)) &&
       cJSON_AddItemToArray(identity, cJSON_CreateString(scopes)) &&
       cJSON_AddItemToArray(identity, cJSON_CreateNumber(expiry->valuedouble)))
   {
      char *material = cJSON_PrintUnformatted(identity);
      if (material)
      {
         rc = aimee_sha256_hex(material, strlen(material), digest);
         OPENSSL_cleanse(material, strlen(material));
         free(material);
      }
   }
   credential_json_cleanse(identity);
   cJSON_Delete(identity);
done:
   credential_json_cleanse(root);
   cJSON_Delete(root);
   return rc;
}

int agent_claude_connection_credentials(const char *registration, agent_claude_credentials_t *out)
{
   if (!out)
      return -1;
   OPENSSL_cleanse(out, sizeof *out);
   int fd = oauth_connection_lock(registration);
   if (fd < 0)
      return -1;
   char document[64 * 1024 + 1], marker[65], client[256] = "", scopes[4096] = "", digest[65] = "";
   oauth_token_response_t source = {0};
   vault_store_read_t fields[] = {{"oauth", document, sizeof document, 0},
                                  {CODEX_SOURCE_CREDENTIAL, marker, sizeof marker, 0}};
   int rc = -1;
   if (vault_service_read_server_snapshot(registration, fields, 2) != VAULT_OK ||
       !fields[0].present || !document[0] ||
       claude_parse_source(document, &source, client, scopes, digest))
      goto done;
   if (!fields[1].present || strcmp(marker, digest))
   {
      if (oauth_token_store_source(registration, &source, digest))
         goto done;
   }
   const oauth_refresh_profile_t profile = {.client_id = client,
                                            .token_endpoint = CLAUDE_OAUTH_TOKEN_ENDPOINT,
                                            .scope = scopes,
                                            .format = OAUTH_REFRESH_JSON};
   if (oauth_token_get_profile(registration, &profile, 300, out->token, sizeof out->token) ||
       !out->token[0] || strpbrk(out->token, "\r\n"))
      goto done;
   snprintf(out->source_digest, sizeof out->source_digest, "%s", digest);
   rc = 0;
done:
   OPENSSL_cleanse(&source, sizeof source);
   OPENSSL_cleanse(document, sizeof document);
   OPENSSL_cleanse(marker, sizeof marker);
   close(fd);
   if (rc)
      OPENSSL_cleanse(out, sizeof *out);
   return rc;
}

static const char *api_key_auth_prefix(const agent_t *agent)
{
   if (!agent)
      return NULL;
   const char *kind = agent->auth_type;
   if (strcmp(kind, "") && strcmp(kind, "api_key") && strcmp(kind, "bearer") &&
       strcmp(kind, "x-api-key"))
      return NULL;
   if (!strcmp(kind, "x-api-key") || !strcmp(agent->provider, "anthropic"))
      return "x-api-key: ";
   if (!strcmp(agent->provider, "gemini") && (!kind[0] || !strcmp(kind, "api_key")))
      return "x-goog-api-key: ";
   return "Authorization: Bearer ";
}

static int credential_target_digest(const agent_t *agent, const char *domain, char digest[65])
{
   if (!agent || !domain)
      return -1;
   const char *registration = agent->registration[0] ? agent->registration : agent->name;
   if (!registration[0] || strlen(registration) >= 128)
      return -1;
   /* Competing credential headers cannot replace the exact Vault source. Other
    * account/project selectors remain part of the immutable target commitment. */
   for (const char *line = agent->extra_headers; line && *line;)
   {
      const char *next = strchr(line, '\n');
      const char *start = line;
      while (*start == ' ' || *start == '\t')
         start++;
      const char *colon = strchr(start, ':');
      if (colon && (!next || colon < next))
      {
         const char *end = colon;
         while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
         size_t length = (size_t)(end - start);
         if ((length == 13 && !strncasecmp(start, "authorization", length)) ||
             (length == 9 && !strncasecmp(start, "x-api-key", length)) ||
             (length == 14 && !strncasecmp(start, "x-goog-api-key", length)))
            return -1;
      }
      line = next ? next + 1 : NULL;
   }
   const char *parts[] = {domain,          registration,     agent->provider,
                          agent->endpoint, agent->auth_type, agent->extra_headers};
   size_t capacity = 1;
   for (size_t i = 0; i < sizeof parts / sizeof *parts; i++)
      capacity += strlen(parts[i]) + 32;
   char *wire = malloc(capacity);
   if (!wire)
      return -1;
   size_t used = 0;
   for (size_t i = 0; i < sizeof parts / sizeof *parts; i++)
   {
      int count = snprintf(wire + used, capacity - used, "%zu:%s", strlen(parts[i]), parts[i]);
      if (count < 0 || (size_t)count >= capacity - used)
      {
         OPENSSL_cleanse(wire, capacity);
         free(wire);
         return -1;
      }
      used += (size_t)count;
   }
   int rc = aimee_sha256_hex(wire, used, digest);
   OPENSSL_cleanse(wire, capacity);
   free(wire);
   return rc;
}

static int api_target_digest(const agent_t *agent, char digest[65])
{
   return agent && api_key_auth_prefix(agent)
              ? credential_target_digest(agent, "vault-api-key-v1", digest)
              : -1;
}
static int claude_target_digest(const agent_t *agent, char digest[65])
{
   return agent && !strcmp(agent->auth_type, "claude-oauth") &&
                  !strcmp(agent->provider, "anthropic")
              ? credential_target_digest(agent, "vault-claude-oauth-v1", digest)
              : -1;
}
int agent_set_request_claude_creds_for(const agent_t *agent,
                                       const agent_claude_credentials_t *credentials)
{
   OPENSSL_cleanse(g_request_claude_token, sizeof g_request_claude_token);
   g_request_claude_registration[0] = g_request_claude_target[0] = 0;
   if (!credentials)
      return 0;
   char target[65];
   if (claude_target_digest(agent, target) || !credentials->token[0] ||
       strpbrk(credentials->token, "\r\n"))
      return -1;
   snprintf(g_request_claude_token, sizeof g_request_claude_token, "%s", credentials->token);
   snprintf(g_request_claude_registration, sizeof g_request_claude_registration, "%s",
            agent->registration[0] ? agent->registration : agent->name);
   snprintf(g_request_claude_target, sizeof g_request_claude_target, "%s", target);
   return 0;
}

int agent_api_connection_credentials(const agent_t *agent, agent_api_credentials_t *out)
{
   if (!out)
      return -1;
   OPENSSL_cleanse(out, sizeof *out);
   if (!agent_get_request_vault_principal()[0] || api_target_digest(agent, out->target_digest))
   {
      OPENSSL_cleanse(out, sizeof *out);
      return -1;
   }
   const char *registration = agent->registration[0] ? agent->registration : agent->name;
   int rc = -1;
   if (vault_service_inject_api_key(agent_get_request_vault_principal(), registration, out->api_key,
                                    sizeof out->api_key, time(NULL)) == VAULT_OK &&
       out->api_key[0] && !strpbrk(out->api_key, "\r\n") &&
       !credential_source_digest(out->api_key, "vault-api-key-v1", out->target_digest,
                                 out->source_digest))
      rc = 0;
   if (rc)
      OPENSSL_cleanse(out, sizeof *out);
   return rc;
}

int agent_set_request_api_credentials_for(const agent_t *agent,
                                          const agent_api_credentials_t *credentials)
{
   OPENSSL_cleanse(g_request_api_key, sizeof g_request_api_key);
   g_request_api_registration[0] = g_request_api_target[0] = 0;
   if (!credentials)
      return 0;
   char target[65], source[65];
   if (!credentials->api_key[0] || strpbrk(credentials->api_key, "\r\n") ||
       api_target_digest(agent, target) || strcmp(target, credentials->target_digest) ||
       credential_source_digest(credentials->api_key, "vault-api-key-v1", target, source) ||
       strcmp(source, credentials->source_digest))
      return -1;
   snprintf(g_request_api_key, sizeof g_request_api_key, "%s", credentials->api_key);
   snprintf(g_request_api_registration, sizeof g_request_api_registration, "%s",
            agent->registration[0] ? agent->registration : agent->name);
   snprintf(g_request_api_target, sizeof g_request_api_target, "%s", target);
   return 0;
}

int agent_codex_oauth_current_token(char *buf, size_t capacity)
{
   if (!buf || !capacity)
      return -1;
   buf[0] = 0;
   agent_codex_credentials_t credentials;
   int rc = agent_codex_connection_credentials(CODEX_OAUTH_STORE, &credentials);
   if (!rc && strlen(credentials.token) < capacity)
      snprintf(buf, capacity, "%s", credentials.token);
   else
      rc = -1;
   OPENSSL_cleanse(&credentials, sizeof credentials);
   return rc;
}

static int resolve_non_codex_auth(const agent_t *agent, char *buf, size_t buf_len)
{
   buf[0] = '\0';
   g_request_auth_error[0] = '\0'; /* reset the per-turn explicit-error channel (D6) */
   const char *auth_type = agent->auth_type;
   if (strcmp(agent->provider, "anthropic") == 0 &&
       (!auth_type[0] || strcmp(auth_type, "bearer") == 0 || strcmp(auth_type, "api_key") == 0))
      auth_type = "x-api-key";

   if (strcmp(auth_type, "none") == 0)
      return 0;

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
   int have_primary_key = 0;
   if (agent->registration[0])
      have_primary_key =
          vault_service_inject_api_key(agent_get_request_vault_principal(), agent->registration,
                                       primary_key, sizeof(primary_key), time(NULL)) == VAULT_OK &&
          primary_key[0];
   if (!have_primary_key)
      have_primary_key =
          vault_service_inject_api_key(agent_get_request_vault_principal(), agent->name,
                                       primary_key, sizeof(primary_key), time(NULL)) == VAULT_OK &&
          primary_key[0];

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
         OPENSSL_cleanse(token, sizeof(token));
         return 0;
      }
   }

   return 0; /* no auth needed (e.g., local Ollama) */
}

/* Match complete header lines, rather than finding another field's substring. */
static int account_header(const char *headers, char value[128])
{
   int found = 0;
   for (const char *line = headers; line && *line;)
   {
      const char *end = strchr(line, '\n');
      if (!end)
         end = line + strlen(line);
      if ((size_t)(end - line) >= 19 && !strncasecmp(line, "ChatGPT-Account-ID:", 19))
      {
         if (found++)
            return -1;
         const char *start = line + 19;
         while (start < end && (*start == ' ' || *start == '\t'))
            start++;
         const char *last = end;
         while (last > start && (last[-1] == '\r' || last[-1] == ' ' || last[-1] == '\t'))
            last--;
         size_t length = (size_t)(last - start);
         if (!length || length >= 128)
            return -1;
         memcpy(value, start, length);
         value[length] = 0;
      }
      line = *end ? end + 1 : NULL;
   }
   return found;
}
/* Preserve the caller's beta capabilities while requiring the subscription
 * authentication capability exactly once. No credential is added to this block. */
int agent_claude_oauth_headers(const agent_t *agent, char *headers, size_t capacity)
{
   if (!agent || strcmp(agent->auth_type, "claude-oauth"))
      return 0;
   if (!headers || !capacity || strnlen(headers, capacity) == capacity)
      return -1;
   const char *required = "oauth-2025-04-20";
   char *beta_end = NULL;
   int present = 0;
   for (char *line = headers; *line;)
   {
      char *end = strchr(line, '\n');
      if (!end)
         end = line + strlen(line);
      char *key = line;
      while (key < end && (*key == ' ' || *key == '\t'))
         key++;
      char *colon = memchr(key, ':', (size_t)(end - key));
      char *key_end = colon;
      while (key_end && key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t'))
         key_end--;
      if (colon && key_end - key == 14 && !strncasecmp(key, "anthropic-beta", 14))
      {
         if (beta_end)
            return -1;
         beta_end = end;
         while (beta_end > colon &&
                (beta_end[-1] == '\r' || beta_end[-1] == ' ' || beta_end[-1] == '\t'))
            beta_end--;
         for (char *value = colon + 1; value < beta_end;)
         {
            char *next = memchr(value, ',', (size_t)(beta_end - value));
            if (!next)
               next = beta_end;
            char *last = next;
            while (value < last && (*value == ' ' || *value == '\t'))
               value++;
            while (last > value && (last[-1] == ' ' || last[-1] == '\t'))
               last--;
            if ((size_t)(last - value) == strlen(required) &&
                !memcmp(value, required, strlen(required)))
            {
               if (++present > 1)
                  return -1;
            }
            value = next == beta_end ? beta_end : next + 1;
         }
      }
      line = *end ? end + 1 : end;
   }
   if (present)
      return 0;
   if (beta_end)
   {
      char *value_start = beta_end;
      while (value_start > headers && value_start[-1] != ':' && value_start[-1] != '\n')
         value_start--;
      while (value_start < beta_end && (*value_start == ' ' || *value_start == '\t'))
         value_start++;
      int separator = value_start != beta_end;
      size_t extra = strlen(required) + (size_t)separator;
      if (strlen(headers) + extra >= capacity)
         return -1;
      memmove(beta_end + extra, beta_end, strlen(beta_end) + 1);
      if (separator)
         *beta_end = ',';
      memcpy(beta_end + separator, required, strlen(required));
   }
   else
   {
      const char *line = "anthropic-beta: oauth-2025-04-20";
      if (strlen(headers) + strlen(line) + 2 >= capacity)
         return -1;
      append_header_line(headers, capacity, line);
   }
   return 0;
}

int agent_resolve_auth_headers(const agent_t *agent, char *auth, size_t auth_capacity,
                               char *headers, size_t headers_capacity)
{
   if (!agent || !auth || !auth_capacity || !headers || !headers_capacity)
      return -1;
   auth[0] = 0;
   g_request_auth_error[0] = 0;
   build_base_extra_headers(agent, headers, headers_capacity);
   const char *registration = agent->registration[0] ? agent->registration : agent->name;
   if (g_request_api_key[0] && !strcmp(registration, g_request_api_registration))
   {
      char target[65];
      if (api_target_digest(agent, target) || strcmp(target, g_request_api_target))
      {
         snprintf(g_request_auth_error, sizeof g_request_auth_error,
                  "selected Vault API-key binding does not match this connection");
         return -1;
      }
      char expected_headers[sizeof agent->extra_headers + 256];
      build_base_extra_headers(agent, expected_headers, sizeof expected_headers);
      if (strlen(expected_headers) >= headers_capacity || strcmp(expected_headers, headers))
      {
         OPENSSL_cleanse(expected_headers, sizeof expected_headers);
         OPENSSL_cleanse(headers, headers_capacity);
         return -1;
      }
      OPENSSL_cleanse(expected_headers, sizeof expected_headers);
      int length =
          snprintf(auth, auth_capacity, "%s%s", api_key_auth_prefix(agent), g_request_api_key);
      if (length < 0 || (size_t)length >= auth_capacity)
      {
         OPENSSL_cleanse(auth, auth_capacity);
         return -1;
      }
      return 0;
   }
   if (!strcmp(agent->auth_type, "claude-oauth"))
   {
      agent_claude_credentials_t credentials = {0};
      char target[65], expected[sizeof agent->extra_headers + 256];
      int rc = -1;
      if (claude_target_digest(agent, target))
         goto claude_done;
      build_base_extra_headers(agent, expected, sizeof expected);
      if (strlen(expected) >= headers_capacity || strcmp(expected, headers))
         goto claude_done;
      if (g_request_claude_token[0] && !strcmp(registration, g_request_claude_registration))
      {
         if (strcmp(target, g_request_claude_target))
            goto claude_done;
         snprintf(credentials.token, sizeof credentials.token, "%s", g_request_claude_token);
      }
      else if (agent_claude_connection_credentials(registration, &credentials))
         goto claude_done;
      int length = snprintf(auth, auth_capacity, "Authorization: Bearer %s", credentials.token);
      if (length >= 0 && (size_t)length < auth_capacity &&
          !agent_claude_oauth_headers(agent, headers, headers_capacity))
         rc = 0;
   claude_done:
      OPENSSL_cleanse(&credentials, sizeof credentials);
      OPENSSL_cleanse(expected, sizeof expected);
      if (rc)
      {
         OPENSSL_cleanse(auth, auth_capacity);
         OPENSSL_cleanse(headers, headers_capacity);
         snprintf(g_request_auth_error, sizeof g_request_auth_error,
                  "selected Vault Claude OAuth credential or connection binding is unavailable");
      }
      return rc;
   }
   if (strcmp(agent->auth_type, "codex-oauth"))
      return resolve_non_codex_auth(agent, auth, auth_capacity);
   agent_codex_credentials_t credentials = {0};
   int rc = -1;
   if (agent_request_codex_token_matches(agent))
   {
      snprintf(credentials.token, sizeof credentials.token, "%s", g_request_codex_token);
      snprintf(credentials.account_id, sizeof credentials.account_id, "%s",
               g_request_codex_account_id);
   }
   else if (agent_codex_connection_credentials(codex_registration(agent), &credentials))
      goto done;
   if (strpbrk(credentials.token, "\r\n") || strpbrk(credentials.account_id, "\r\n"))
      goto done;
   char configured[128] = "";
   int has_account = account_header(headers, configured);
   if (has_account < 0 ||
       (has_account && credentials.account_id[0] && strcmp(configured, credentials.account_id)))
      goto done;
   int has_originator = 0;
   for (const char *line = headers; line && *line;)
   {
      if (!strncasecmp(line, "originator:", 11))
         has_originator = 1;
      const char *next = strchr(line, '\n');
      line = next ? next + 1 : NULL;
   }
   if (!has_originator)
   {
      if (strlen(headers) + sizeof("originator: codex_cli_rs\n") >= headers_capacity)
         goto done;
      append_header_line(headers, headers_capacity, "originator: codex_cli_rs");
   }
   if (credentials.account_id[0] && !has_account)
   {
      char line[160];
      snprintf(line, sizeof line, "ChatGPT-Account-ID: %s", credentials.account_id);
      if (strlen(headers) + strlen(line) + 2 >= headers_capacity)
         goto done;
      append_header_line(headers, headers_capacity, line);
   }
   int length = snprintf(auth, auth_capacity, "Authorization: Bearer %s", credentials.token);
   if (length >= 0 && (size_t)length < auth_capacity)
      rc = 0;
done:
   OPENSSL_cleanse(&credentials, sizeof credentials);
   if (rc)
   {
      OPENSSL_cleanse(auth, auth_capacity);
      snprintf(
          g_request_auth_error, sizeof g_request_auth_error,
          "Codex credential/account is unavailable or inconsistent for selected connection '%s'%s",
          codex_registration(agent),
          oauth_token_reauth_required(codex_registration(agent))
              ? "; re-authenticate this connection"
              : "");
   }
   return rc;
}
