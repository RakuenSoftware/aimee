/* test_oauth_pkce.c: unit tests for the PKCE primitives and OAuth token flow. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "oauth_pkce.h"
#include "oauth_flow.h"
#include "db1.h" /* db1_secret_* — plant/verify legacy plaintext for the migration test */
#include "aimee.h"
#include "platform_test_util.h"

static void test_base64url_rfc4648_vectors(void)
{
   /* RFC 4648 §10 plus RFC 7515 Appendix C.2 (for non-standard bytes). */
   struct
   {
      const char *in;
      const char *want;
   } cases[] = {
       {"", ""},           {"f", "Zg"},          {"fo", "Zm8"},          {"foo", "Zm9v"},
       {"foob", "Zm9vYg"}, {"fooba", "Zm9vYmE"}, {"foobar", "Zm9vYmFy"},
   };

   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      char out[32];
      int rc = oauth_pkce_base64url_encode((const unsigned char *)cases[i].in, strlen(cases[i].in),
                                           out, sizeof(out));
      assert(rc == 0);
      assert(strcmp(out, cases[i].want) == 0);
   }

   /* Verify that '+' and '/' never appear: encode bytes that map to the
    * two non-standard indices (62, 63) in base64url. 0x3e,0x3f,0xbf →
    * standard "Pj+/", base64url → "Pj-_". */
   unsigned char raw[] = {0x3e, 0x3f, 0xbf};
   char out[16];
   assert(oauth_pkce_base64url_encode(raw, sizeof(raw), out, sizeof(out)) == 0);
   assert(strcmp(out, "Pj-_") == 0);
}

static void test_base64url_buffer_too_small(void)
{
   unsigned char raw[] = {0xab, 0xcd, 0xef};
   char out[4]; /* need 5: "q83v" + NUL */
   assert(oauth_pkce_base64url_encode(raw, sizeof(raw), out, sizeof(out)) == -1);

   char ok[5];
   assert(oauth_pkce_base64url_encode(raw, sizeof(raw), ok, sizeof(ok)) == 0);
   assert(strcmp(ok, "q83v") == 0);
}

static void test_s256_challenge_rfc7636_vector(void)
{
   /* RFC 7636 Appendix B. */
   const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
   const char *want = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

   char out[OAUTH_PKCE_CHALLENGE_LEN + 1];
   assert(oauth_pkce_s256_challenge(verifier, out, sizeof(out)) == 0);
   assert(strcmp(out, want) == 0);
   assert(strlen(out) == OAUTH_PKCE_CHALLENGE_LEN);
}

static void test_s256_rejects_bad_verifier_length(void)
{
   char out[64];
   char short_v[OAUTH_PKCE_VERIFIER_MIN]; /* 42 chars + NUL — too short */
   memset(short_v, 'a', sizeof(short_v) - 1);
   short_v[sizeof(short_v) - 1] = '\0';
   assert(oauth_pkce_s256_challenge(short_v, out, sizeof(out)) == -1);

   char long_v[OAUTH_PKCE_VERIFIER_MAX + 2]; /* 129 chars — too long */
   memset(long_v, 'a', sizeof(long_v) - 1);
   long_v[sizeof(long_v) - 1] = '\0';
   assert(oauth_pkce_s256_challenge(long_v, out, sizeof(out)) == -1);

   /* Too-small output buffer is rejected. */
   char legit[OAUTH_PKCE_VERIFIER_MIN + 1];
   memset(legit, 'a', sizeof(legit) - 1);
   legit[sizeof(legit) - 1] = '\0';
   char tiny[OAUTH_PKCE_CHALLENGE_LEN]; /* one short of required */
   assert(oauth_pkce_s256_challenge(legit, tiny, sizeof(tiny)) == -1);
}

static int is_unreserved(char c)
{
   return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~');
}

static void test_generate_verifier_length_and_charset(void)
{
   char buf[OAUTH_PKCE_VERIFIER_MAX + 1];

   /* Valid lengths produce a correctly terminated, unreserved-only string. */
   size_t lengths[] = {OAUTH_PKCE_VERIFIER_MIN, 64, OAUTH_PKCE_VERIFIER_MAX};
   for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
   {
      memset(buf, 0xff, sizeof(buf));
      assert(oauth_pkce_generate_verifier(buf, lengths[i]) == 0);
      assert(strlen(buf) == lengths[i]);
      for (size_t k = 0; k < lengths[i]; k++)
         assert(is_unreserved(buf[k]));
   }

   /* Out-of-range lengths are rejected. */
   assert(oauth_pkce_generate_verifier(buf, OAUTH_PKCE_VERIFIER_MIN - 1) == -1);
   assert(oauth_pkce_generate_verifier(buf, OAUTH_PKCE_VERIFIER_MAX + 1) == -1);
   assert(oauth_pkce_generate_verifier(NULL, 64) == -1);
}

static void test_generate_verifier_distinct(void)
{
   /* Two successive calls should almost never collide. With 43 chars from
    * a 66-symbol alphabet the space is ~2^260 — a collision here means the
    * RNG is broken. */
   char a[OAUTH_PKCE_VERIFIER_MIN + 1];
   char b[OAUTH_PKCE_VERIFIER_MIN + 1];
   assert(oauth_pkce_generate_verifier(a, OAUTH_PKCE_VERIFIER_MIN) == 0);
   assert(oauth_pkce_generate_verifier(b, OAUTH_PKCE_VERIFIER_MIN) == 0);
   assert(strcmp(a, b) != 0);
}

static void test_generate_verifier_roundtrips_with_s256(void)
{
   char verifier[OAUTH_PKCE_VERIFIER_MAX + 1];
   char challenge[OAUTH_PKCE_CHALLENGE_LEN + 1];
   assert(oauth_pkce_generate_verifier(verifier, 96) == 0);
   assert(oauth_pkce_s256_challenge(verifier, challenge, sizeof(challenge)) == 0);
   assert(strlen(challenge) == OAUTH_PKCE_CHALLENGE_LEN);
   /* Challenge uses only base64url unreserved chars (no '+', '/', '='). */
   for (size_t i = 0; i < OAUTH_PKCE_CHALLENGE_LEN; i++)
   {
      char c = challenge[i];
      assert(c != '+' && c != '/' && c != '=');
   }
}

static void test_build_auth_url_minimal(void)
{
   oauth_pkce_auth_request_t req = {
       .authorize_url = "https://example.com/oauth/authorize",
       .client_id = "abc123",
       .redirect_uri = "http://localhost:8765/callback",
       .code_challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
   };
   char out[512];
   int n = oauth_pkce_build_auth_url(&req, out, sizeof(out));
   assert(n > 0);
   assert((size_t)n == strlen(out));

   const char *want = "https://example.com/oauth/authorize"
                      "?response_type=code"
                      "&client_id=abc123"
                      "&redirect_uri=http%3A%2F%2Flocalhost%3A8765%2Fcallback"
                      "&code_challenge=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
                      "&code_challenge_method=S256";
   assert(strcmp(out, want) == 0);
}

static void test_build_auth_url_with_scope_state_and_existing_query(void)
{
   oauth_pkce_auth_request_t req = {
       .authorize_url = "https://example.com/authorize?tenant=acme",
       .client_id = "client id", /* space forces encoding */
       .redirect_uri = "https://app/cb",
       .scope = "repo read:user",
       .state = "xyz=1",
       .code_challenge = "ABC_-xyz",
   };
   char out[512];
   assert(oauth_pkce_build_auth_url(&req, out, sizeof(out)) > 0);

   /* Uses '&' as the initial separator since authorize_url already has a query. */
   assert(strstr(out, "?tenant=acme&response_type=code") != NULL);
   /* Space in client_id is percent-encoded. */
   assert(strstr(out, "client_id=client%20id") != NULL);
   /* Scope space encoded; colon encoded per RFC 3986. */
   assert(strstr(out, "scope=repo%20read%3Auser") != NULL);
   /* State '=' encoded. */
   assert(strstr(out, "state=xyz%3D1") != NULL);
}

static void test_build_auth_url_rejects_missing_fields(void)
{
   char out[256];
   oauth_pkce_auth_request_t missing_client = {
       .authorize_url = "https://x/",
       .client_id = "",
       .redirect_uri = "https://y/",
       .code_challenge = "abc",
   };
   assert(oauth_pkce_build_auth_url(&missing_client, out, sizeof(out)) == -1);

   oauth_pkce_auth_request_t missing_challenge = {
       .authorize_url = "https://x/",
       .client_id = "id",
       .redirect_uri = "https://y/",
       .code_challenge = NULL,
   };
   assert(oauth_pkce_build_auth_url(&missing_challenge, out, sizeof(out)) == -1);

   assert(oauth_pkce_build_auth_url(NULL, out, sizeof(out)) == -1);
}

static void test_build_auth_url_buffer_too_small(void)
{
   oauth_pkce_auth_request_t req = {
       .authorize_url = "https://example.com/oauth/authorize",
       .client_id = "abc",
       .redirect_uri = "https://app/cb",
       .code_challenge = "ABC",
   };
   char tiny[16];
   assert(oauth_pkce_build_auth_url(&req, tiny, sizeof(tiny)) == -1);
}

int main(void)
{
   char tmp_template[] = "/tmp/aimee-oauth-pkce-XXXXXX";
   char *tmp_home = mkdtemp(tmp_template);
   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   assert(tmp_home != NULL);
   assert(setenv("HOME", tmp_home, 1) == 0);

   test_base64url_rfc4648_vectors();
   test_base64url_buffer_too_small();
   test_s256_challenge_rfc7636_vector();
   test_s256_rejects_bad_verifier_length();
   test_generate_verifier_length_and_charset();
   test_generate_verifier_distinct();
   test_generate_verifier_roundtrips_with_s256();
   test_build_auth_url_minimal();
   test_build_auth_url_with_scope_state_and_existing_query();
   test_build_auth_url_rejects_missing_fields();
   test_build_auth_url_buffer_too_small();

   /* --- oauth_token_parse_response: happy path --- */
   {
      const char *json = "{"
                         "\"access_token\": \"eyABC\","
                         "\"refresh_token\": \"rTOK\","
                         "\"token_type\": \"Bearer\","
                         "\"expires_in\": 3600"
                         "}";
      oauth_token_response_t resp;
      int rc = oauth_token_parse_response(json, &resp);
      assert(rc == 0);
      assert(strcmp(resp.access_token, "eyABC") == 0);
      assert(strcmp(resp.refresh_token, "rTOK") == 0);
      assert(strcmp(resp.token_type, "Bearer") == 0);
      assert(resp.expires_in == 3600);
      assert(resp.expires_at > (long)time(NULL)); /* should be in the future */
   }

   /* --- oauth_token_parse_response: missing access_token → error --- */
   {
      const char *json = "{\"token_type\": \"Bearer\"}";
      oauth_token_response_t resp;
      int rc = oauth_token_parse_response(json, &resp);
      assert(rc != 0);
   }

   /* --- oauth_token_parse_response: malformed JSON → error --- */
   {
      oauth_token_response_t resp;
      int rc = oauth_token_parse_response("not json", &resp);
      assert(rc != 0);
   }

   /* --- oauth_token_parse_response: pre-computed expires_at is accepted --- */
   {
      const char *json = "{"
                         "\"access_token\": \"eyXYZ\","
                         "\"expires_at\": 9999999999"
                         "}";
      oauth_token_response_t resp;
      int rc = oauth_token_parse_response(json, &resp);
      assert(rc == 0);
      assert(resp.expires_at == 9999999999L);
   }

   /* --- oauth_token_store + oauth_token_load round-trip (in-memory secret backend) --- */
   {
      /* Use a unique test client name so we don't clash with real secrets */
      const char *client = "test-oauth-unit";

      oauth_token_response_t to_store;
      memset(&to_store, 0, sizeof(to_store));
      snprintf(to_store.access_token, sizeof(to_store.access_token), "access-abc");
      snprintf(to_store.refresh_token, sizeof(to_store.refresh_token), "refresh-xyz");
      to_store.expires_at = (long)time(NULL) + 7200; /* 2 hours from now */

      int rc = oauth_token_store(client, &to_store);
      assert(rc == 0);

      char loaded[1024];
      rc = oauth_token_load(client, loaded, sizeof(loaded));
      assert(rc == 0);
      assert(strcmp(loaded, "access-abc") == 0);

      /* Clean up */
      oauth_token_remove(client);

      /* After removal, load should fail */
      rc = oauth_token_load(client, loaded, sizeof(loaded));
      assert(rc != 0);
   }

   /* --- legacy plaintext (db1/secrets) is migrated into the vault on read --- */
   {
      const char *client = "test-oauth-migrate";
      char key[256];
      /* Plant a legacy plaintext access token + future expiry the old way. */
      snprintf(key, sizeof(key), OAUTH_KEY_ACCESS_TOKEN, client);
      assert(db1_secret_store(key, "legacy-access-token") == 0);
      char ea[32];
      snprintf(ea, sizeof(ea), "%ld", (long)time(NULL) + 7200);
      snprintf(key, sizeof(key), OAUTH_KEY_EXPIRES_AT, client);
      assert(db1_secret_store(key, ea) == 0);

      /* Load migrates it to the encrypted vault and returns the value. */
      char loaded[1024];
      assert(oauth_token_load(client, loaded, sizeof(loaded)) == 0);
      assert(strcmp(loaded, "legacy-access-token") == 0);

      /* Both legacy plaintext keys that load touches (access + expires) are
       * scrubbed once migrated. (The refresh token is only read during a network
       * refresh, so it migrates lazily then — not on a plain load.) */
      char chk[256];
      snprintf(key, sizeof(key), OAUTH_KEY_ACCESS_TOKEN, client);
      assert(db1_secret_load(key, chk, sizeof(chk)) != 0);
      snprintf(key, sizeof(key), OAUTH_KEY_EXPIRES_AT, client);
      assert(db1_secret_load(key, chk, sizeof(chk)) != 0);

      /* ...and a subsequent load still works (now from the vault, no plaintext). */
      assert(oauth_token_load(client, loaded, sizeof(loaded)) == 0);
      assert(strcmp(loaded, "legacy-access-token") == 0);

      oauth_token_remove(client);
      assert(oauth_token_load(client, loaded, sizeof(loaded)) != 0);
   }

   /* --- oauth_token_get: returns -1 when no token stored --- */
   {
      oauth_token_remove("nonexistent-client-test");
      char buf[256];
      int rc = oauth_token_get("nonexistent-client-test", NULL, NULL, 60, buf, sizeof(buf));
      assert(rc != 0);
   }

   if (old_home)
   {
      assert(setenv("HOME", old_home, 1) == 0);
      free(old_home);
   }
   else
   {
      assert(unsetenv("HOME") == 0);
   }

   platform_test_rmrf(tmp_template);

   printf("test_oauth_pkce: all tests passed\n");
   return 0;
}
