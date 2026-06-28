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

   snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
   (void)system(cmd);
   printf("PASS: codex REAUTH_REQUIRED marker round-trip + clear-on-reauth (D6)\n");
   return 0;
}
