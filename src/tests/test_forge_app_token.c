/* test_forge_app_token.c: unit tests for the GitHub App installation-token
 * provider (forge_app_token.h).
 *
 * Covers the pure internals (JWT shape, token-response parse, refresh
 * decision) plus the full mint/cache/refresh/fail/unconfigured loop through
 * the agent_http mock (tests/support/mock_agent_http.c).
 *
 * The mock's post handler signature is the 6-arg agent_http_post form
 *   (url, auth_header, body, char **response_buf, int timeout_ms, extra_headers)
 * and the return value is the HTTP status; forge_app_token.c treats a 2xx
 * status with a non-NULL response body as success, so the handlers below
 * return 201 and strdup() a JSON body into *response_buf. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* setenv, timegm, strptime */
#endif

#include "forge_app_token.h"

#include "cJSON.h"
#include "runtime_secret.h"
#include "support/mock_agent_http.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A real, parseable throwaway 2048-bit RSA private key (generated with
 * `openssl genrsa 2048`). Never used against any live forge. */
static const char *const TEST_PEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEuwIBADANBgkqhkiG9w0BAQEFAASCBKUwggShAgEAAoIBAQCnt+q6hHq0uOzS\n"
    "aIXIXbhvxHGKSovGWIAXgIKeW2TQVBOYP8ngs/MaZ9r54p3SexjkQGhhagQMSm+c\n"
    "gl7+nvMuBF4omGDAKltCqLwKc4kZ0bYlhy8Yq2q65hZ4hNxzr9Z09c1JNOXcJNYQ\n"
    "mqGJlIRw/RDa7+kLwL0TdFpkDFQ00LeI8qpVWOfVfJop60mXy7OWpDAAsQVyfyX4\n"
    "dsDGpJSdUpk20eZIZZ5HtzE8dF02X/V+XU7RUYoz02IQFEE4w6EebKbcIflKVMYA\n"
    "PfcNbDMtIQzGzd2rl9FNnY4J69leMW33FNibRDAETTIlktCDjg8fw1y/TSgCT2cb\n"
    "2Cm1mi3vAgMBAAECgf9qddQJkQ5SqQ+qf65sARGR4KAxqCARRxwHzwsaeekEVFob\n"
    "ymHawF8P49ybwb1sXbbvK/MV4rWvF5DBSAAEn+C81Qjvu8muMTS8m17BQ5VDTlrQ\n"
    "d37tqeGVwXeCCNPmmzcGBwDH90vw8XDwGTdwnVV0Yy7PynmjfDSIpNK7rxfEbbBq\n"
    "bFeop2FiZmUbw1zrQWFOc6kBpL7WZYspX4PHnI449MM8KRz7pWqrSmiifMC/nMGf\n"
    "9q0xALQO6GltmbLCEUsgMqD8ED8jc18tSEhsIZPjXd0erhqSGm/gZG3dItne/AMZ\n"
    "B1KxyBkkvsAVWzeNo6gT0YkAIsq473l/3WOomQ0CgYEA3al+GYpk3Outmz2xvrVQ\n"
    "r6hsixJ+DDlPgoxbq4iU1k9bNqMQJtfXbRGXSw5EGz46kk0hPYsOUuOUKAAdkvpd\n"
    "r9hop6pmIbPXQ9acs+0o/1jTcmikG3lwrngAv1nWOOqn/37LUIDgbZ8041HbwBlv\n"
    "EwHZm3++pnHhWk4SJ30QLG0CgYEAwbMq509KoIPBKjuIW1YVuDbzKudrSmK1u7+7\n"
    "fo1rYAr+f2u0mJ3VPhsENRE1KS3r4Nl0TnhkdwU9stZxXIv2AkxUumCxPOHMQvI/\n"
    "ptcke3qYCXQmQWgtbOjJOWkW3Rei0mCXo1ImpV5Apb5HhrX3RQflo5nfLYEbCC4f\n"
    "+xhDkksCgYAePngUKAvnEMkZO2u7J6YgsYxN1XIZXOB6YYhIeVRFgYJijBEyG5Ur\n"
    "LpFEDmhAh1caiyeT7BtCOAcJBisC2OJbkd2FsvxIcO9YNDohWqSTYp1HKPvrO8Ci\n"
    "LYF8mldeYLYXaEd5bnwwuN5QfeTL6yx6nXABhYaP0036MljLxoakaQKBgQCYd3Ez\n"
    "9YPmaQ8pMQcZ7d7Wy9oIUXRwbtZh3H+3E5YLWVwN7DeRUdtCMX3UT6EqsszShhg4\n"
    "lCdwUB3KoWVF1Z1lHbQrqGSaaZmgsJJNv1cmIs990YEzRs9KxMlveTrX+Pze3808\n"
    "bzOgQ1pbnDUs4hqqqZamej3j0ZX3kGb3/JdjlQKBgEUdu68Zx5m98jVFaCr4AlmR\n"
    "g6QT/52psfwsF9pJ5OzqNzND43bau4iBoKALfpUFY0okJe/PFTJYEp7IUxsqXTUL\n"
    "l5GxGlnJne6BoJKmAFYlqVV1lvUhl5Wla6cGWc6GCaWL2Jq+uyEdNBtzJKYMHjBO\n"
    "PR5uI9mY8/aHkafBUI/Q\n"
    "-----END PRIVATE KEY-----\n";

/* --- tiny URL-safe base64 decoder (no padding) --------------------------- */

/* Decode a base64url segment (alphabet A-Za-z0-9-_ , no '=' padding) into a
 * freshly malloc'd, NUL-terminated buffer. Returns the byte length via *out_len
 * (excluding the NUL) or NULL on a bad character. Caller frees. */
static unsigned char *b64url_decode(const char *in, size_t *out_len)
{
   static signed char tbl[256];
   static int inited = 0;
   if (!inited)
   {
      for (int i = 0; i < 256; i++)
         tbl[i] = -1;
      const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      for (int i = 0; alpha[i]; i++)
         tbl[(unsigned char)alpha[i]] = (signed char)i;
      inited = 1;
   }

   size_t n = strlen(in);
   unsigned char *out = malloc(n + 1); /* always enough for the 3/4 expansion */
   if (!out)
      return NULL;

   size_t oi = 0;
   uint32_t buf = 0;
   int bits = 0;
   for (size_t i = 0; i < n; i++)
   {
      signed char v = tbl[(unsigned char)in[i]];
      if (v < 0)
      {
         free(out);
         return NULL;
      }
      buf = (buf << 6) | v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         out[oi++] = (unsigned char)((buf >> bits) & 0xFF);
      }
   }
   out[oi] = '\0';
   if (out_len)
      *out_len = oi;
   return out;
}

/* --- test 1: JWT shape --------------------------------------------------- */

static void test_jwt_shape(void)
{
   char *jwt = forge_app_build_jwt("123456", TEST_PEM, 1700000000L, 540L);
   assert(jwt != NULL && "forge_app_build_jwt should mint a JWT for a valid key");

   /* exactly two '.' separators -> three segments */
   int dots = 0;
   for (const char *p = jwt; *p; p++)
      if (*p == '.')
         dots++;
   assert(dots == 2 && "JWT must have exactly three dot-separated segments");

   /* isolate the middle (payload) segment */
   const char *first = strchr(jwt, '.');
   assert(first);
   const char *second = strchr(first + 1, '.');
   assert(second);
   size_t mid_len = (size_t)(second - (first + 1));
   char *mid = malloc(mid_len + 1);
   assert(mid);
   memcpy(mid, first + 1, mid_len);
   mid[mid_len] = '\0';

   size_t dec_len = 0;
   unsigned char *payload = b64url_decode(mid, &dec_len);
   assert(payload != NULL && dec_len > 0 && "payload segment must base64url-decode");

   cJSON *root = cJSON_Parse((const char *)payload);
   assert(root != NULL && "decoded payload must be valid JSON");

   const cJSON *iss = cJSON_GetObjectItemCaseSensitive(root, "iss");
   const cJSON *iat = cJSON_GetObjectItemCaseSensitive(root, "iat");
   const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
   assert(cJSON_IsString(iss) && strcmp(iss->valuestring, "123456") == 0 && "iss must be app_id");
   assert(cJSON_IsNumber(iat) && (long)iat->valuedouble == 1700000000L && "iat must match");
   assert(cJSON_IsNumber(exp) && (long)exp->valuedouble == 1700000540L && "exp must be iat+ttl");

   cJSON_Delete(root);
   free(payload);
   free(mid);
   free(jwt);

   /* a bad PEM yields NULL */
   char *bad = forge_app_build_jwt("123456", "not a key", 1700000000L, 540L);
   assert(bad == NULL && "a non-PEM key must fail JWT minting");
}

/* --- test 2: token-response parse ---------------------------------------- */

static long iso_to_unix(int y, int mo, int d, int h, int mi, int s)
{
   struct tm tm_v;
   memset(&tm_v, 0, sizeof(tm_v));
   tm_v.tm_year = y - 1900;
   tm_v.tm_mon = mo - 1;
   tm_v.tm_mday = d;
   tm_v.tm_hour = h;
   tm_v.tm_min = mi;
   tm_v.tm_sec = s;
   return (long)timegm(&tm_v);
}

static void test_parse_token_response(void)
{
   char tok[256];
   long exp = 0;

   /* good response */
   const char *ok = "{\"token\":\"ghs_abc123\",\"expires_at\":\"2099-01-02T03:04:05Z\"}";
   assert(forge_app_parse_token_response(ok, tok, sizeof(tok), &exp) == 0 &&
          "well-formed token response must parse");
   assert(strcmp(tok, "ghs_abc123") == 0 && "token field must be extracted");
   assert(exp == iso_to_unix(2099, 1, 2, 3, 4, 5) && "expires_at must be the ISO unix time");

   /* missing expires_at -> -1 */
   tok[0] = '\0';
   exp = 0;
   assert(forge_app_parse_token_response("{\"token\":\"x\"}", tok, sizeof(tok), &exp) == -1 &&
          "a response without expires_at must fail");

   /* malformed JSON -> -1 */
   assert(forge_app_parse_token_response("{not json", tok, sizeof(tok), &exp) == -1 &&
          "malformed JSON must fail");
}

/* --- test 3: needs_refresh ----------------------------------------------- */

static void test_needs_refresh(void)
{
   /* 600 < 700 -> still fresh */
   assert(forge_app_token_needs_refresh(1000, 600, 300) == 0 &&
          "before the skew window, no refresh is needed");
   /* now == expires - skew -> refresh */
   assert(forge_app_token_needs_refresh(1000, 700, 300) == 1 &&
          "at the skew boundary, refresh is needed");
   /* now past the boundary -> refresh */
   assert(forge_app_token_needs_refresh(1000, 800, 300) == 1 &&
          "past the skew window, refresh is needed");
}

/* --- test 4: mint / cache / refresh / fail / unconfigured ---------------- */

static int g_post_calls = 0;
static const char *g_next_token = "ghs_first";
static const char *g_next_expires = "2099-01-01T00:00:00Z";
static int g_next_status = 201;
static int g_return_body = 1;

static int recording_post_handler(const char *url, const char *auth_header, const char *body,
                                  char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   g_post_calls++;
   if (response_buf && g_return_body)
   {
      char json[256];
      snprintf(json, sizeof(json), "{\"token\":\"%s\",\"expires_at\":\"%s\"}", g_next_token,
               g_next_expires);
      *response_buf = strdup(json);
   }
   return g_next_status;
}

static void test_mint_cache_refresh(void)
{
   char buf[256];

   setenv("AIMEE_FORGE_APP_ID", "123456", 1);
   assert(runtime_secret_store("AIMEE_FORGE_APP_PRIVATE_KEY", TEST_PEM) == 0);
   setenv("AIMEE_FORGE_APP_INSTALLATION_ID", "987", 1);
   setenv("AIMEE_FORGE_API_BASE", "http://mock.local", 1);

   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);

   /* --- first mint: one HTTP call, token from the mock --- */
   forge_app_token_reset_cache();
   g_post_calls = 0;
   g_next_token = "ghs_first";
   g_next_expires = "2099-01-01T00:00:00Z";
   g_next_status = 201;
   g_return_body = 1;

   buf[0] = '\0';
   assert(forge_app_token_get(buf, sizeof(buf)) == 1 && "first mint must succeed");
   assert(strcmp(buf, "ghs_first") == 0 && "first token must come from the mock");
   assert(g_post_calls == 1 && "first mint must issue exactly one HTTP call");

   /* --- second get: served from cache, no new HTTP call --- */
   buf[0] = '\0';
   assert(forge_app_token_get(buf, sizeof(buf)) == 1 && "cached get must succeed");
   assert(strcmp(buf, "ghs_first") == 0 && "cached get returns the same token");
   assert(g_post_calls == 1 && "cached get must NOT issue another HTTP call");

   /* --- forced refresh: cache reset, mock now returns a new token --- */
   forge_app_token_reset_cache();
   g_next_token = "ghs_second";
   buf[0] = '\0';
   assert(forge_app_token_get(buf, sizeof(buf)) == 1 && "refresh mint must succeed");
   assert(strcmp(buf, "ghs_second") == 0 && "refresh must return the new token");
   assert(g_post_calls == 2 && "refresh must issue a second HTTP call");

   /* --- failure path: non-2xx status fails closed, buf untouched --- */
   forge_app_token_reset_cache();
   g_next_status = 401;
   g_return_body = 1;
   strcpy(buf, "PRESERVED");
   assert(forge_app_token_get(buf, sizeof(buf)) == -1 && "a non-2xx response must fail closed");
   assert(strcmp(buf, "PRESERVED") == 0 && "buf must be left untouched on failure");

   /* restore a healthy handler status for tidiness */
   g_next_status = 201;

   /* --- unconfigured path: APP_* unset -> fall back (return 0) --- */
   unsetenv("AIMEE_FORGE_APP_ID");
   runtime_secret_remove("AIMEE_FORGE_APP_PRIVATE_KEY");
   unsetenv("AIMEE_FORGE_APP_INSTALLATION_ID");
   forge_app_token_reset_cache();
   assert(forge_app_token_get(buf, sizeof(buf)) == 0 &&
          "unconfigured App env must fall back (return 0)");

   mock_agent_http_reset();
   unsetenv("AIMEE_FORGE_API_BASE");
   runtime_secret_clear();
}

int main(void)
{
   test_jwt_shape();
   test_parse_token_response();
   test_needs_refresh();
   test_mint_cache_refresh();

   printf("forge_app_token: all tests passed\n");
   return 0;
}
