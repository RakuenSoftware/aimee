/* forge_app_token.c: GitHub App installation-token mint/cache/refresh.
 * See forge_app_token.h.
 *
 * When AIMEE_FORGE_APP_* is configured this mints a GitHub App installation
 * token from an App ID + RSA private key: build a short-lived RS256 App JWT,
 * POST it to /app/installations/<id>/access_tokens, and cache the returned
 * installation token until just before its expiry. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strptime, timegm */
#endif

#include "forge_app_token.h"

#include "cJSON.h"
#include "log.h"
#include "oauth_pkce.h"
#include "runtime_secret.h"

/* agent_http_post (agent_exec.h): performs the HTTPS POST. Forward-declared to
 * keep this TU off the heavyweight agent_exec.h / agent_types.h include chain;
 * the signature must match agent_exec.h exactly. Calling the real symbol lets
 * the test mock (tests/support/mock_agent_http.c) intercept it. */
extern int agent_http_post(const char *url, const char *auth_header, const char *body,
                           char **response_buf, int timeout_ms, const char *extra_headers);

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORGE_APP_LOG "forge_app"

/* Cached installation token. Tokens are short-lived; the cache holds at most
 * one because a hub has a single App identity. */
#define FORGE_APP_TOKEN_MAX       512
#define FORGE_APP_PRIVATE_KEY_MAX 4096

/* --- JWT minting --------------------------------------------------------- */

/* Load an RSA private key from Vault-sourced PEM text. Filesystem paths are not
 * accepted: persistent credential material belongs only in Vault. */
static EVP_PKEY *forge_app_load_privkey(const char *pem)
{
   if (!pem || !strstr(pem, "-----BEGIN"))
      return NULL;

   BIO *bio = BIO_new_mem_buf(pem, -1);
   if (!bio)
      return NULL;
   EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
   BIO_free(bio);
   return pkey;
}

/* RS256-sign `data` (signing_input bytes) with `pkey`; base64url-encode the
 * signature into a freshly malloc'd string (caller frees). NULL on error. */
static char *forge_app_sign_rs256(EVP_PKEY *pkey, const unsigned char *data, size_t data_len)
{
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   if (!ctx)
      return NULL;

   unsigned char *sig = NULL;
   char *encoded = NULL;
   size_t sig_len = 0;

   if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey) != 1)
      goto done;
   if (EVP_DigestSignUpdate(ctx, data, data_len) != 1)
      goto done;
   /* First call with NULL buffer determines the signature length. */
   if (EVP_DigestSignFinal(ctx, NULL, &sig_len) != 1 || sig_len == 0)
      goto done;
   sig = malloc(sig_len);
   if (!sig)
      goto done;
   if (EVP_DigestSignFinal(ctx, sig, &sig_len) != 1)
      goto done;

   /* base64url-no-pad expands to at most 4*ceil(n/3) chars; +1 for NUL. */
   size_t enc_cap = ((sig_len + 2) / 3) * 4 + 1;
   encoded = malloc(enc_cap);
   if (!encoded)
      goto done;
   if (oauth_pkce_base64url_encode(sig, sig_len, encoded, enc_cap) != 0)
   {
      free(encoded);
      encoded = NULL;
      goto done;
   }

done:
   free(sig);
   EVP_MD_CTX_free(ctx);
   return encoded;
}

/* Concatenate a + "." + b into a fresh malloc'd string (caller frees). */
static char *forge_app_join_dot(const char *a, const char *b)
{
   size_t la = strlen(a);
   size_t lb = strlen(b);
   char *out = malloc(la + lb + 2);
   if (!out)
      return NULL;
   memcpy(out, a, la);
   out[la] = '.';
   memcpy(out + la + 1, b, lb);
   out[la + lb + 1] = '\0';
   return out;
}

char *forge_app_build_jwt(const char *app_id, const char *pem, long iat, long ttl_secs)
{
   if (!app_id || !app_id[0] || !pem || !pem[0])
      return NULL;

   char *header_b64 = NULL;
   char *payload_b64 = NULL;
   char *signing_input = NULL;
   char *sig_b64 = NULL;
   char *jwt = NULL;
   EVP_PKEY *pkey = NULL;

   /* Compact, fixed JOSE header. */
   static const char header_json[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";

   char payload_json[256];
   int pn = snprintf(payload_json, sizeof(payload_json), "{\"iat\":%ld,\"exp\":%ld,\"iss\":\"%s\"}",
                     iat, iat + ttl_secs, app_id);
   if (pn < 0 || (size_t)pn >= sizeof(payload_json))
      return NULL;

   /* base64url each segment (each fits within 4*ceil(n/3)+1). */
   size_t hcap = ((sizeof(header_json) - 1 + 2) / 3) * 4 + 1;
   header_b64 = malloc(hcap);
   size_t pcap = (((size_t)pn + 2) / 3) * 4 + 1;
   payload_b64 = malloc(pcap);
   if (!header_b64 || !payload_b64)
      goto done;
   if (oauth_pkce_base64url_encode((const unsigned char *)header_json, sizeof(header_json) - 1,
                                   header_b64, hcap) != 0)
      goto done;
   if (oauth_pkce_base64url_encode((const unsigned char *)payload_json, (size_t)pn, payload_b64,
                                   pcap) != 0)
      goto done;

   signing_input = forge_app_join_dot(header_b64, payload_b64);
   if (!signing_input)
      goto done;

   pkey = forge_app_load_privkey(pem);
   if (!pkey)
      goto done;

   sig_b64 =
       forge_app_sign_rs256(pkey, (const unsigned char *)signing_input, strlen(signing_input));
   if (!sig_b64)
      goto done;

   jwt = forge_app_join_dot(signing_input, sig_b64);

done:
   free(header_b64);
   free(payload_b64);
   free(signing_input);
   free(sig_b64);
   EVP_PKEY_free(pkey);
   return jwt;
}

/* --- Response parsing ---------------------------------------------------- */

int forge_app_parse_token_response(const char *json, char *tok_out, size_t tok_cap,
                                   long *expires_at)
{
   if (!json || !tok_out || tok_cap == 0 || !expires_at)
      return -1;

   cJSON *root = cJSON_Parse(json);
   if (!root)
      return -1;

   int rc = -1;
   const cJSON *tok = cJSON_GetObjectItemCaseSensitive(root, "token");
   const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
   if (!cJSON_IsString(tok) || !tok->valuestring || !tok->valuestring[0])
      goto done;
   if (!cJSON_IsString(exp) || !exp->valuestring || !exp->valuestring[0])
      goto done;

   struct tm tm_exp;
   memset(&tm_exp, 0, sizeof(tm_exp));
   if (!strptime(exp->valuestring, "%Y-%m-%dT%H:%M:%SZ", &tm_exp))
      goto done;

   snprintf(tok_out, tok_cap, "%s", tok->valuestring);
   *expires_at = (long)timegm(&tm_exp);
   rc = 0;

done:
   cJSON_Delete(root);
   return rc;
}

/* --- Refresh decision / configuration gate ------------------------------- */

int forge_app_token_needs_refresh(long expires_at, long now, long skew_secs)
{
   return now >= expires_at - skew_secs;
}

int forge_app_token_configured(void)
{
   const char *id = getenv("AIMEE_FORGE_APP_ID");
   const char *inst = getenv("AIMEE_FORGE_APP_INSTALLATION_ID");
   return (id && id[0] && runtime_secret_has("AIMEE_FORGE_APP_PRIVATE_KEY") && inst && inst[0]) ? 1
                                                                                                : 0;
}

/* --- Token cache + mint -------------------------------------------------- */

static struct
{
   char token[FORGE_APP_TOKEN_MAX];
   long expires_at;
} g_forge_app_cache = {{0}, 0};

static pthread_mutex_t g_forge_app_lock = PTHREAD_MUTEX_INITIALIZER;

void forge_app_token_reset_cache(void)
{
   pthread_mutex_lock(&g_forge_app_lock);
   g_forge_app_cache.token[0] = '\0';
   g_forge_app_cache.expires_at = 0;
   pthread_mutex_unlock(&g_forge_app_lock);
}

int forge_app_token_get(char *tok_out, size_t tok_cap)
{
   if (!tok_out || tok_cap == 0)
      return -1;
   if (!forge_app_token_configured())
      return 0;

   const char *app_id = getenv("AIMEE_FORGE_APP_ID");
   const char *installation_id = getenv("AIMEE_FORGE_APP_INSTALLATION_ID");
   const char *api_base_env = getenv("AIMEE_FORGE_API_BASE");

   /* Normalise the API base: default to github.com, strip a trailing slash. */
   char api_base[512];
   snprintf(api_base, sizeof(api_base), "%s",
            (api_base_env && api_base_env[0]) ? api_base_env : "https://api.github.com");
   size_t base_len = strlen(api_base);
   while (base_len > 0 && api_base[base_len - 1] == '/')
   {
      api_base[base_len - 1] = '\0';
      base_len--;
   }

   pthread_mutex_lock(&g_forge_app_lock);

   /* Serve the cache while it is comfortably valid (5-min skew). */
   if (g_forge_app_cache.token[0] &&
       !forge_app_token_needs_refresh(g_forge_app_cache.expires_at, time(NULL), 300))
   {
      snprintf(tok_out, tok_cap, "%s", g_forge_app_cache.token);
      pthread_mutex_unlock(&g_forge_app_lock);
      return 1;
   }

   char private_key[FORGE_APP_PRIVATE_KEY_MAX];
   if (!runtime_secret_get("AIMEE_FORGE_APP_PRIVATE_KEY", private_key, sizeof(private_key)))
   {
      pthread_mutex_unlock(&g_forge_app_lock);
      return -1;
   }

   /* Mint a new installation token. iat is backdated 60s for clock skew; ttl is
    * 540s (GitHub caps App JWTs at 600s). */
   long now = time(NULL);
   char *jwt = forge_app_build_jwt(app_id, private_key, now - 60, 540);
   runtime_secret_wipe(private_key, sizeof(private_key));
   if (!jwt)
   {
      LOG_ERROR(FORGE_APP_LOG, "failed to build App JWT (check AIMEE_FORGE_APP_PRIVATE_KEY)");
      pthread_mutex_unlock(&g_forge_app_lock);
      return -1;
   }

   char url[768];
   snprintf(url, sizeof(url), "%s/app/installations/%s/access_tokens", api_base, installation_id);

   char auth_header[FORGE_APP_TOKEN_MAX + 64];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", jwt);

   /* NB: agent_http_post takes a single auth header. GitHub recommends
    * Accept: application/vnd.github+json, but the access_tokens endpoint works
    * with just the bearer, so we omit the extra Accept header here. */
   char *resp = NULL;
   int status = agent_http_post(url, auth_header, "", &resp, 15000, NULL);

   runtime_secret_wipe(auth_header, sizeof(auth_header));
   runtime_secret_wipe(jwt, strlen(jwt));
   free(jwt);

   if (status < 200 || status >= 300 || !resp)
   {
      LOG_ERROR(FORGE_APP_LOG, "installation-token request failed (http %d)", status);
      free(resp);
      pthread_mutex_unlock(&g_forge_app_lock);
      return -1;
   }

   char token[FORGE_APP_TOKEN_MAX];
   long expires_at = 0;
   if (forge_app_parse_token_response(resp, token, sizeof(token), &expires_at) != 0)
   {
      LOG_ERROR(FORGE_APP_LOG, "failed to parse installation-token response");
      free(resp);
      pthread_mutex_unlock(&g_forge_app_lock);
      return -1;
   }
   free(resp);

   snprintf(g_forge_app_cache.token, sizeof(g_forge_app_cache.token), "%s", token);
   g_forge_app_cache.expires_at = expires_at;
   snprintf(tok_out, tok_cap, "%s", token);
   runtime_secret_wipe(token, sizeof(token));

   /* Never log the token itself — only the refresh event + expiry. */
   LOG_INFO(FORGE_APP_LOG, "refreshed GitHub App installation token (expires_at=%ld)", expires_at);

   pthread_mutex_unlock(&g_forge_app_lock);
   return 1;
}
