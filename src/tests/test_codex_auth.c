/* test_codex_auth.c: codex_local_auth parses ~/.codex/auth.json (OAuth tokens.*
 * shape + top-level fallback) from the HOME-rooted path. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "codex_auth.h"

static char g_home[256];

static void set_home_with_auth(const char *json)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee_codex_test_%d", (int)getpid());
   char dir[320];
   snprintf(dir, sizeof(dir), "%s/.codex", g_home);
   mkdir(g_home, 0700);
   mkdir(dir, 0700);
   char path[400];
   snprintf(path, sizeof(path), "%s/auth.json", dir);
   if (json)
   {
      FILE *f = fopen(path, "wb");
      assert(f);
      fputs(json, f);
      fclose(f);
   }
   else
   {
      remove(path);
   }
   setenv("HOME", g_home, 1);
}

int main(void)
{
   char token[4096], account[256];

   /* OAuth shape: tokens.{access_token,account_id}. */
   set_home_with_auth("{\"auth_mode\":\"chatgpt\",\"tokens\":{\"access_token\":\"AT-123\","
                      "\"account_id\":\"acct-xyz\",\"refresh_token\":\"r\"}}");
   assert(codex_local_auth(token, sizeof(token), account, sizeof(account)) == 0);
   assert(strcmp(token, "AT-123") == 0);
   assert(strcmp(account, "acct-xyz") == 0);

   /* Top-level access_token fallback; account optional. */
   set_home_with_auth("{\"access_token\":\"TOP-456\"}");
   assert(codex_local_auth(token, sizeof(token), account, sizeof(account)) == 0);
   assert(strcmp(token, "TOP-456") == 0);
   assert(account[0] == '\0');

   /* Missing token -> failure, outputs cleared. */
   set_home_with_auth("{\"tokens\":{\"account_id\":\"acct-only\"}}");
   assert(codex_local_auth(token, sizeof(token), account, sizeof(account)) == -1);
   assert(token[0] == '\0');

   /* No file -> failure. */
   set_home_with_auth(NULL);
   assert(codex_local_auth(token, sizeof(token), account, sizeof(account)) == -1);

   /* NULL out-pointers tolerated. */
   set_home_with_auth("{\"tokens\":{\"access_token\":\"AT\"}}");
   assert(codex_local_auth(NULL, 0, NULL, 0) == 0);

   char cmd[400];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_home);
   if (system(cmd) != 0)
      fprintf(stderr, "cleanup warn\n");

   printf("codex_auth: all tests passed\n");
   return 0;
}
