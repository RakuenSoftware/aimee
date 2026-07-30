/* test_forge_credentials.c — the per-workspace forge-token broker
 * (workspace-resource-plane §4): install/get, TTL expiry, scope lattice,
 * revoke-zeroes-and-drops, and the exec-env injection (token rides the child
 * environment, never the command line / disk). */
#include "forge_credentials.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static const char *g_static_token;

static int static_token_get(char *out, size_t cap)
{
   if (!g_static_token || !out || cap == 0 || strlen(g_static_token) >= cap)
      return 0;
   snprintf(out, cap, "%s", g_static_token);
   return 1;
}

static int env_has(char **envp, const char *prefix, const char *want_suffix)
{
   for (int i = 0; envp && envp[i]; i++)
      if (strncmp(envp[i], prefix, strlen(prefix)) == 0)
         return want_suffix ? (strcmp(envp[i] + strlen(prefix), want_suffix) == 0) : 1;
   return 0;
}

int main(void)
{
   const long T0 = 1000000;
   const char *WS = "/home/me/proj";

   forge_cred_revoke_all();
   assert(forge_cred_count() == 0);

   /* install + get within TTL */
   assert(forge_cred_install(WS, "ghs_secrettoken", "project", 60, T0) == 0);
   assert(forge_cred_count() == 1);
   char tok[256];
   assert(forge_cred_get(WS, T0 + 30, tok, sizeof(tok)) == 0);
   assert(strcmp(tok, "ghs_secrettoken") == 0);

   char sc[64];
   assert(forge_cred_scope(WS, T0 + 30, sc, sizeof(sc)) == 0);
   assert(strcmp(sc, "project") == 0);

   /* expiry: at/after expires_epoch the token is gone */
   assert(forge_cred_get(WS, T0 + 60, tok, sizeof(tok)) == -1);
   assert(tok[0] == '\0');
   assert(forge_cred_get(WS, T0 + 1000, tok, sizeof(tok)) == -1);

   /* re-install replaces (and refreshes TTL); still one slot */
   assert(forge_cred_install(WS, "ghs_newtoken", "workspace", 120, T0 + 100) == 0);
   assert(forge_cred_count() == 1);
   assert(forge_cred_get(WS, T0 + 150, tok, sizeof(tok)) == 0);
   assert(strcmp(tok, "ghs_newtoken") == 0);

   /* unknown workspace → miss */
   assert(forge_cred_get("/other/ws", T0 + 150, tok, sizeof(tok)) == -1);

   /* bad args rejected */
   assert(forge_cred_install(NULL, "t", "project", 60, T0) == -1);
   assert(forge_cred_install(WS, "", "project", 60, T0) == -1);
   assert(forge_cred_install(WS, "t", "bogus", 60, T0) == -1);
   assert(forge_cred_install(WS, "t", "project", 0, T0) == -1);

   /* scope lattice: broader credential satisfies narrower requirement */
   assert(forge_cred_scope_allows("global", "project") == 1);
   assert(forge_cred_scope_allows("workspace", "project") == 1);
   assert(forge_cred_scope_allows("project", "project") == 1);
   assert(forge_cred_scope_allows("project", "workspace") == 0); /* narrower can't widen */
   assert(forge_cred_scope_allows("user", "project") == 0);
   assert(forge_cred_scope_allows("global", "user") == 1);
   assert(forge_cred_scope_allows(NULL, "project") == 0);
   assert(forge_cred_scope_allows("project", "bogus") == 0);

   /* exec-env injection: token rides the child environment (never argv/disk) */
   {
      char *parent[] = {(char *)"PATH=/usr/bin", (char *)"GH_TOKEN=stale_inherited",
                        (char *)"HOME=/home/me", NULL};
      char **envp = forge_cred_build_env(WS, T0 + 150, parent, "/opt/aimee/git-askpass");
      assert(envp != NULL);
      /* our token wins; the inherited stale GH_TOKEN was dropped */
      assert(env_has(envp, "GH_TOKEN=", "ghs_newtoken"));
      int gh = 0;
      for (int i = 0; envp[i]; i++)
         if (strncmp(envp[i], "GH_TOKEN=", 9) == 0)
            gh++;
      assert(gh == 1);
      assert(env_has(envp, "GIT_ASKPASS=", "/opt/aimee/git-askpass"));
      assert(env_has(envp, "GIT_TERMINAL_PROMPT=", "0"));
      assert(env_has(envp, "PATH=", "/usr/bin")); /* parent entries preserved */
      assert(env_has(envp, "HOME=", "/home/me"));
      forge_cred_free_env(envp);
   }

   /* no askpass shim → only GH_TOKEN added */
   {
      char *parent[] = {(char *)"PATH=/usr/bin", NULL};
      char **envp = forge_cred_build_env(WS, T0 + 150, parent, NULL);
      assert(envp != NULL);
      assert(env_has(envp, "GH_TOKEN=", "ghs_newtoken"));
      assert(!env_has(envp, "GIT_ASKPASS=", NULL));
      forge_cred_free_env(envp);
   }

   /* expired / absent → no env (caller falls back to ambient creds) */
   assert(forge_cred_build_env(WS, T0 + 9999, NULL, NULL) == NULL);
   assert(forge_cred_build_env("/nope", T0 + 150, NULL, NULL) == NULL);

   /* revoke: token gone, slot freed */
   forge_cred_revoke(WS);
   assert(forge_cred_count() == 0);
   assert(forge_cred_get(WS, T0 + 150, tok, sizeof(tok)) == -1);
   forge_cred_revoke(WS); /* idempotent */

   /* revoke_all */
   assert(forge_cred_install("/a", "t1", "project", 60, T0) == 0);
   assert(forge_cred_install("/b", "t2", "user", 60, T0) == 0);
   assert(forge_cred_count() == 2);
   forge_cred_revoke_all();
   assert(forge_cred_count() == 0);

   /* --- server-held forge identity (§6): vault-sourced, for instance-held
    * workspaces driven by a surface that supplies no token. --- */
   {
      unsetenv("AIMEE_FORGE_TOKEN");
      unsetenv("AIMEE_FORGE_SCOPE");
      char st[256], ss[64];
      /* unconfigured → no identity, no env */
      assert(forge_cred_server_identity(st, sizeof(st), ss, sizeof(ss)) == 0);
      assert(st[0] == '\0' && ss[0] == '\0');
      assert(forge_cred_build_server_env(environ, "/opt/aimee/git-askpass") == NULL);

      /* A raw credential env is never a runtime source. */
      setenv("AIMEE_FORGE_TOKEN", "must-not-be-consumed", 1);
      assert(forge_cred_server_identity(st, sizeof(st), ss, sizeof(ss)) == 0);

      /* A registered vault provider supplies the identity + default scope. */
      g_static_token = "ghs_serverApp";
      forge_cred_register_static_token_provider(static_token_get);
      assert(forge_cred_server_identity(st, sizeof(st), ss, sizeof(ss)) == 1);
      assert(strcmp(st, "ghs_serverApp") == 0);
      assert(strcmp(ss, "workspace") == 0);

      /* explicit scope honored */
      setenv("AIMEE_FORGE_SCOPE", "global", 1);
      assert(forge_cred_server_identity(st, sizeof(st), ss, sizeof(ss)) == 1);
      assert(strcmp(ss, "global") == 0);

      /* the server identity rides the exec env exactly like a per-workspace one */
      char **envp = forge_cred_build_server_env(environ, "/opt/aimee/git-askpass");
      assert(envp != NULL);
      assert(env_has(envp, "GH_TOKEN=", "ghs_serverApp"));
      assert(env_has(envp, "GIT_ASKPASS=", "/opt/aimee/git-askpass"));
      forge_cred_free_env(envp);

      g_static_token = NULL;
      unsetenv("AIMEE_FORGE_TOKEN");
      unsetenv("AIMEE_FORGE_SCOPE");
   }

   printf("forge_credentials: all tests passed\n");
   return 0;
}
