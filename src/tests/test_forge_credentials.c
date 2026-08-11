/* test_forge_credentials.c — the server's own forge identity.
 *
 * What this file used to cover was mostly the per-workspace BROKER: install a
 * token against a workspace id, read it back, scope it, expire it, revoke it,
 * build an exec env from it. That mechanism is gone — aimee git proxies through
 * aimee's own vaulted credential and there is no brokered token to hold — so
 * those tests went with it rather than being left to test nothing.
 *
 * What remains is the part that is still real: where the SERVER's identity
 * comes from, and the rule that a raw environment variable is never a runtime
 * credential source. */
#include "modules/git/forge_credentials.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_static_token;

static int static_token_get(char *out, size_t cap)
{
   if (!g_static_token || !out || cap == 0)
      return 0;
   snprintf(out, cap, "%s", g_static_token);
   return 1;
}

int main(void)
{
   unsetenv("AIMEE_FORGE_TOKEN");
   unsetenv("AIMEE_FORGE_SCOPE");

   char token[256], scope[64];

   /* Unconfigured: no identity at all. */
   assert(forge_cred_server_identity(token, sizeof(token), scope, sizeof(scope)) == 0);
   assert(token[0] == '\0' && scope[0] == '\0');

   /* A RAW CREDENTIAL ENV IS NEVER A RUNTIME SOURCE. Reading one would let
    * anything that can set an environment variable supply the server's identity,
    * which is why the vault provider below is the only accepted route. */
   setenv("AIMEE_FORGE_TOKEN", "must-not-be-consumed", 1);
   assert(forge_cred_server_identity(token, sizeof(token), scope, sizeof(scope)) == 0);

   /* A registered vault provider supplies the identity and a default scope. */
   g_static_token = "ghs_serverApp";
   forge_cred_register_static_token_provider(static_token_get);
   assert(forge_cred_server_identity(token, sizeof(token), scope, sizeof(scope)) == 1);
   assert(strcmp(token, "ghs_serverApp") == 0);
   assert(strcmp(scope, "workspace") == 0);

   /* An explicit scope is honoured. */
   setenv("AIMEE_FORGE_SCOPE", "global", 1);
   assert(forge_cred_server_identity(token, sizeof(token), scope, sizeof(scope)) == 1);
   assert(strcmp(scope, "global") == 0);

   g_static_token = NULL;
   unsetenv("AIMEE_FORGE_TOKEN");
   unsetenv("AIMEE_FORGE_SCOPE");

   printf("forge_credentials: all tests passed\n");
   return 0;
}
