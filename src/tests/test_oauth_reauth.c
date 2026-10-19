/* test_oauth_reauth.c — D6 REAUTH_REQUIRED credential state.
 *
 * Proves the marker the codex refresh path sets on an IdP rejection round-trips
 * through the vault and is cleared by the next successful token store (a fresh
 * operator re-auth), so a recovered codex credential no longer reports as
 * needing re-auth. */
#include "oauth_flow.h"
#include "vault_service.h"
#include "vault_server_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* Mirrors OAUTH_VCRED_REAUTH in oauth_tokens.c (the marker cred slot). */
#define REAUTH_VCRED "oauth_reauth_required"

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-oauthreauth-test-%d", (int)getpid());
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(cmd) == 0);
   setenv("AIMEE_HOME", home, 1);
   vault_server_key_reset_for_test();

   /* Fresh credential: not flagged. */
   assert(oauth_token_reauth_required("codex") == 0);

   /* Simulate the refresh-rejection path setting the marker. */
   assert(vault_service_set_server("codex", REAUTH_VCRED, "1") == VAULT_OK);
   assert(oauth_token_reauth_required("codex") == 1 && "marker should read back set");

   /* A successful token store (operator re-auth) clears the marker. */
   oauth_token_response_t resp;
   memset(&resp, 0, sizeof(resp));
   snprintf(resp.access_token, sizeof(resp.access_token), "tok-after-reauth");
   snprintf(resp.refresh_token, sizeof(resp.refresh_token), "rt-after-reauth");
   resp.expires_at = time(NULL) + 3600;
   assert(oauth_token_store("codex", &resp) == 0);
   assert(oauth_token_reauth_required("codex") == 0 &&
          "a successful token store must clear REAUTH_REQUIRED");

   /* And the fresh token is now usable (no refresh needed). */
   char buf[256] = "";
   assert(oauth_token_get("codex", NULL, NULL, 0, buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "tok-after-reauth") == 0 && "stored access token should load");

   /* The high-level getter must honor the same validity contract as load. */
   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   snprintf(buf, sizeof buf, "%s", "stale caller buffer");
   assert(oauth_token_get("codex", NULL, NULL, 0, buf, sizeof buf) == -1 && !buf[0]);
   resp.expires_at = time(NULL) + 3600;
   assert(!oauth_token_store("codex", &resp));
   assert(vault_service_set_server("codex", REAUTH_VCRED, "1") == VAULT_OK);
   assert(oauth_token_get("codex", NULL, NULL, 0, buf, sizeof buf) == -1 && !buf[0]);

   /* Exercise the real refresh transport with a private fake curl. No network
    * or operator credential is involved; the real Vault stores each result. */
   char curl_path[320], test_path[8192];
   const char *inherited_path = getenv("PATH");
   char *saved_path = inherited_path ? strdup(inherited_path) : NULL;
   snprintf(curl_path, sizeof curl_path, "%s/curl", home);
   FILE *curl = fopen(curl_path, "w");
   assert(curl);
   assert(fputs("#!/bin/sh\ncat >/dev/null\nprintf '%s' \"$AIMEE_TEST_OAUTH_RESPONSE\"\n", curl) >=
          0);
   int close_rc = fclose(curl);
   assert(!close_rc);
   assert(!chmod(curl_path, 0700));
   assert(snprintf(test_path, sizeof test_path, "%s:%s", home,
                   saved_path ? saved_path : "/usr/bin:/bin") < (int)sizeof test_path);
   assert(!setenv("PATH", test_path, 1));

   resp.expires_at = time(NULL) + 300;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE", "{\"error\":\"temporarily_unavailable\"}\n503", 1));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   assert(!strcmp(buf, "tok-after-reauth"));
   assert(!oauth_token_reauth_required("codex"));

   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   assert(oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                          sizeof buf) == -1 &&
          !buf[0]);

   resp.expires_at = time(NULL) + 300;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE", "{\"error\":\"invalid_grant\"}\n400", 1));
   assert(oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                          sizeof buf) == -1 &&
          !buf[0]);
   assert(oauth_token_reauth_required("codex"));

   /* Successful refresh replaces an expired access token and clears the marker
    * only through a successful store (a new login first clears rejection). */
   resp.expires_at = time(NULL) - 10;
   assert(!oauth_token_store("codex", &resp));
   assert(!setenv("AIMEE_TEST_OAUTH_RESPONSE",
                  "{\"access_token\":\"refreshed-fixture\",\"expires_in\":3600}\n200", 1));
   assert(!oauth_token_get("codex", "fixture-client", "https://fixture.invalid/token", 600, buf,
                           sizeof buf));
   assert(!strcmp(buf, "refreshed-fixture"));
   assert(!oauth_token_reauth_required("codex"));

   resp.expires_at = 0;
   assert(!oauth_token_store("codex", &resp));
   assert(!oauth_token_get("codex", NULL, NULL, 0, buf, sizeof buf));
   assert(!strcmp(buf, "tok-after-reauth"));
   assert(oauth_token_get(NULL, NULL, NULL, 0, buf, sizeof buf) == -1 && !buf[0]);

   if (saved_path)
      assert(!setenv("PATH", saved_path, 1));
   else
      assert(!unsetenv("PATH"));
   free(saved_path);
   assert(!unsetenv("AIMEE_TEST_OAUTH_RESPONSE"));

   snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
   (void)system(cmd);
   printf("PASS: OAuth token validity, refresh fallback and REAUTH_REQUIRED (D6)\n");
   return 0;
}
