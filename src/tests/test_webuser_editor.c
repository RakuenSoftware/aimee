/* test_webuser_editor.c — WP-I/WP-K: the per-webuser code-server supervisor's
 * gating, identity validation, and bookkeeping, plus the WP-K editor-env leak
 * gate. The actual code-server spawn is deploy-validated (CI has no code-server
 * binary), so here we exercise the fail-closed paths and prove the launched
 * environment carries NO server secrets. */
#include "webuser_editor.h"

#include "vault_kek_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 1 iff envp has an entry exactly equal to `want`. */
static int env_has(char **envp, const char *want)
{
   for (int i = 0; envp[i]; i++)
      if (strcmp(envp[i], want) == 0)
         return 1;
   return 0;
}

/* 1 iff envp has an entry beginning with `prefix`. */
static int env_has_prefix(char **envp, const char *prefix)
{
   size_t n = strlen(prefix);
   for (int i = 0; envp[i]; i++)
      if (strncmp(envp[i], prefix, n) == 0)
         return 1;
   return 0;
}

/* 1 iff `needle` appears anywhere in any entry (secret-value leak check). */
static int env_contains_substr(char **envp, const char *needle)
{
   for (int i = 0; envp[i]; i++)
      if (strstr(envp[i], needle))
         return 1;
   return 0;
}

/* WP-K leak gate: the editor's env must carry the curated base + HOME + git
 * credential-cache-off, and NONE of the server's own secrets — even when those
 * are present in the process environment the server runs with. */
static void test_editor_env_leak(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-editor-env-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s/ws", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);
   char ws[300];
   snprintf(ws, sizeof(ws), "%s/ws", home);
   setenv("AIMEE_WORKSPACES_DIR", ws, 1);
   vault_kek_cache_clear();

   /* Secrets the live server may legitimately hold in its own environment — the
    * editor (terminal + extensions) must never see any of them. */
   setenv("AIMEE_DB2_URL", "postgres://u:SUPERSECRETPW@db/aimee", 1);
   setenv("AIMEE_SERVER_TOKEN", "tok-LEAKME-123", 1);
   setenv("ANTHROPIC_API_KEY", "sk-ant-LEAKME", 1);

   char userroot[400];
   snprintf(userroot, sizeof(userroot), "%s/ws/webusers/alice", home);
   char **env = webuser_editor_build_env("webuser:alice", userroot);
   assert(env != NULL);

   /* Curated base + pinned HOME. */
   char want_home[460];
   snprintf(want_home, sizeof(want_home), "HOME=%s", userroot);
   assert(env_has(env, want_home));
   assert(env_has_prefix(env, "PATH="));
   assert(env_has_prefix(env, "LANG="));
   assert(env_has(env, "TERM=xterm-256color"));

   /* On-disk credential cache disabled (token never persisted to ~/.git-credentials). */
   assert(env_has(env, "GIT_CONFIG_COUNT=1"));
   assert(env_has(env, "GIT_CONFIG_KEY_0=credential.helper"));
   assert(env_has(env, "GIT_CONFIG_VALUE_0="));

   /* No server environment leaks: neither the variable names nor their values. */
   assert(!env_has_prefix(env, "AIMEE_"));
   assert(!env_has_prefix(env, "ANTHROPIC_API_KEY="));
   assert(!env_contains_substr(env, "SUPERSECRETPW"));
   assert(!env_contains_substr(env, "tok-LEAKME-123"));
   assert(!env_contains_substr(env, "sk-ant-LEAKME"));

   webuser_editor_free_env(env);

   unsetenv("AIMEE_DB2_URL");
   unsetenv("AIMEE_SERVER_TOKEN");
   unsetenv("ANTHROPIC_API_KEY");
}

int main(void)
{
   int port = -1;
   char err[256];

   /* A dummy executable stands in for the code-server binary so the gate logic
    * can be tested without a real editor. It is resolved + cached on the first
    * availability check (AIMEE_WEBCHAT_EDITOR_BIN is tried first). */
   char fakebin[256];
   snprintf(fakebin, sizeof(fakebin), "/tmp/aimee-fake-code-server-%d", (int)getpid());
   FILE *fb = fopen(fakebin, "w");
   assert(fb);
   fputs("#!/bin/sh\nexit 0\n", fb);
   fclose(fb);
   assert(chmod(fakebin, 0755) == 0);
   setenv("AIMEE_WEBCHAT_EDITOR_BIN", fakebin, 1);

   /* Feature ON by default (AIMEE_WEBCHAT_EDITOR unset) when a binary is present. */
   unsetenv("AIMEE_WEBCHAT_EDITOR");
   assert(webuser_editor_available() == 1);

   /* AIMEE_WEBCHAT_EDITOR=0 explicitly disables it. */
   setenv("AIMEE_WEBCHAT_EDITOR", "0", 1);
   assert(webuser_editor_available() == 0);
   /* Disabled → ensure fails closed (returns 0), never spawns. */
   assert(webuser_editor_ensure("webuser:alice", &port, err, sizeof(err)) == 0);

   /* Identity guard runs before the availability check: only webuser: principals
    * may drive an editor. NULL / non-webuser / missing out_port → -1, never a
    * spawn (kept disabled here as belt-and-braces). */
   assert(webuser_editor_ensure("uid:1000", &port, err, sizeof(err)) == -1);
   assert(webuser_editor_ensure(NULL, &port, err, sizeof(err)) == -1);
   assert(webuser_editor_ensure("webuser:bob", NULL, err, sizeof(err)) == -1);

   /* Bookkeeping ops are safe no-ops with no editors running. */
   webuser_editor_touch("webuser:alice");
   webuser_editor_touch(NULL);
   webuser_editor_stop("webuser:alice");
   webuser_editor_stop(NULL);
   assert(webuser_editor_reap_idle(0) == 0);  /* idle_secs<=0 reaps nothing */
   assert(webuser_editor_reap_idle(60) == 0); /* empty registry */
   webuser_editor_shutdown();

   test_editor_env_leak();

   printf("webuser_editor: ok\n");
   return 0;
}
