/* test_cli_session.c: unit tests for cli_session pure-C functions */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "cli_session.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char g_fake_dir[256], g_createlog[300];
static void install_fake_tmux(void)
{
   snprintf(g_fake_dir, sizeof g_fake_dir, "%s/aimee-tmux-%d", platform_tmpdir(), (int)getpid());
   assert(!mkdir(g_fake_dir, 0700));
   snprintf(g_createlog, sizeof g_createlog, "%s/create.log", g_fake_dir);
   char script[320];
   snprintf(script, sizeof script, "%s/tmux", g_fake_dir);
   FILE *f = fopen(script, "w");
   assert(f);
   fprintf(f,
           "#!/bin/sh\ncase \"$1\" in\nhas-session) exit 0 ;;\ncapture-pane) echo 'STATIC OUTPUT'; "
           "exit 0 ;;\nnew-session) shift; if [ \"$FAKE_TMUX_MODE\" = startup ]; then while [ "
           "\"$1\" != /bin/bash ]; do shift; done; exec \"$@\"; fi; for a in \"$@\"; do echo "
           "\"ARG:[$a]\" >> '%s'; done; exit 0 ;;\n*) exit 0 ;;\nesac\n",
           g_createlog);
   assert(!fclose(f) && !chmod(script, 0700));
   const char *path = getenv("PATH");
   char next[4096];
   snprintf(next, sizeof next, "%s:%s", g_fake_dir, path ? path : "");
   assert(!setenv("PATH", next, 1));
}

static int createlog_has(const char *needle)
{
   FILE *f = fopen(g_createlog, "r");
   if (!f)
      return 0;
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return strstr(buf, needle) != NULL;
}

static void test_create_multiword_cli_cmd_single_arg(void)
{
   unlink(g_createlog);
   char quoting_wd[256];
   snprintf(quoting_wd, sizeof quoting_wd, "%s/aimee quoting wd", platform_tmpdir());
   cli_session_t s;
   int rc = cli_session_create(&s, "aimee-quoting-test",
                               "AIMEE_SESSION_ID=web-1 claude --dangerously-skip-permissions",
                               quoting_wd, 0);
   assert(rc == 0);
   assert(createlog_has("ARG:[/bin/bash]"));
   assert(createlog_has("ARG:[-ic]"));
   assert(createlog_has("ARG:[AIMEE_SESSION_ID=web-1 claude --dangerously-skip-permissions]"));
   char expect_wd[300];
   snprintf(expect_wd, sizeof expect_wd, "ARG:[%s]", quoting_wd);
   assert(createlog_has(expect_wd));
   /* No fragment may arrive as its own argument (the word-split regression). */
   assert(!createlog_has("ARG:[AIMEE_SESSION_ID=web-1]"));
   s.active = 0; /* fake tmux: no real session to tear down */
}

static void test_new_session_loads_bashrc(void)
{
   char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char rcfile[320], output[320], command[1024];
   snprintf(rcfile, sizeof(rcfile), "%s/.bashrc", g_fake_dir);
   snprintf(output, sizeof(output), "%s/startup-result", g_fake_dir);
   FILE *f = fopen(rcfile, "w");
   assert(f);
   fputs("export AIMEE_BASHRC_TEST=loaded\n", f);
   fclose(f);
   setenv("HOME", g_fake_dir, 1);
   setenv("FAKE_TMUX_MODE", "startup", 1);
   snprintf(command, sizeof(command), "printf '%%s' \"$AIMEE_BASHRC_TEST\" > '%s'", output);
   cli_session_t session;
   assert(cli_session_create(&session, "startup-test", command, NULL, 0) == 0);
   f = fopen(output, "r");
   assert(f);
   char value[32] = "";
   assert(fgets(value, sizeof(value), f));
   fclose(f);
   assert(strcmp(value, "loaded") == 0);
   if (saved_home)
      setenv("HOME", saved_home, 1);
   else
      unsetenv("HOME");
   free(saved_home);
   setenv("FAKE_TMUX_MODE", "stable", 1);
   unlink(rcfile);
   unlink(output);
}

static void test_make_name_format(void)
{
   char *name = cli_session_make_name("myagent", "coder");
   assert(name != NULL);
   /* Must start with "aimee-myagent-" */
   assert(strncmp(name, "aimee-myagent-", 14) == 0);
   /* Must be at most CLI_SESSION_NAME_MAX-1 chars */
   assert(strlen(name) < CLI_SESSION_NAME_MAX);
   free(name);
}

static void test_make_name_deterministic(void)
{
   char *a = cli_session_make_name("agent1", "review");
   char *b = cli_session_make_name("agent1", "review");
   assert(a && b);
   assert(strcmp(a, b) == 0);
   free(a);
   free(b);
}

static void test_make_name_different_roles(void)
{
   char *a = cli_session_make_name("agent1", "review");
   char *b = cli_session_make_name("agent1", "coder");
   assert(a && b);
   /* Different roles should produce different names */
   assert(strcmp(a, b) != 0);
   free(a);
   free(b);
}

static void test_make_name_sanitizes_chars(void)
{
   char *name = cli_session_make_name("my agent", "role.test:path/x");
   assert(name != NULL);
   /* No spaces, dots, colons, or slashes in name */
   for (const char *p = name; *p; p++)
   {
      assert(*p != ' ');
      assert(*p != '.');
      assert(*p != ':');
      assert(*p != '/');
   }
   free(name);
}

static void test_make_name_null_role(void)
{
   char *name = cli_session_make_name("agent", NULL);
   assert(name != NULL);
   assert(strncmp(name, "aimee-agent-", 12) == 0);
   free(name);
}

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *b = malloc((size_t)n + 1);
   size_t rd = fread(b, 1, (size_t)n, f);
   fclose(f);
   b[rd] = '\0';
   return b;
}

static void test_isolated_home_reclaimed_on_destroy(void)
{
   char shared[128];
   snprintf(shared, sizeof(shared), "%s/aimee-isorm-%d", platform_tmpdir(), (int)getpid());
   char cdir[192], creds[288], json[288];
   snprintf(cdir, sizeof(cdir), "%s/.claude", shared);
   mkdir(shared, 0700);
   mkdir(cdir, 0700);
   snprintf(creds, sizeof(creds), "%s/.credentials.json", cdir);
   snprintf(json, sizeof(json), "%s/.claude.json", shared);
   FILE *f = fopen(creds, "w");
   assert(f);
   fputs("{\"token\":\"x\"}", f);
   fclose(f);
   f = fopen(json, "w");
   assert(f);
   fputs("{\"oauthAccount\":{\"id\":\"a\"}}", f);
   fclose(f);
   setenv("HOME", shared, 1);
   char runtime[160];
   snprintf(runtime, sizeof(runtime), "%s/aimee-isorm-runtime-%d", platform_tmpdir(),
            (int)getpid());
   setenv("AIMEE_OAUTH_RUNTIME_DIR", runtime, 1);

   char home[PATH_MAX];
   cli_session_t s = {0};
   char binding[65];
   memset(binding, 'a', 64);
   binding[64] = 0;
   assert(!cli_session_native_home("claude", "/w/d", "cleanup-source", binding, binding, home,
                                   sizeof home, &s));
   struct stat st;
   assert(stat(home, &st) == 0 && S_ISDIR(st.st_mode)); /* minted on disk */

   /* A never-created (active=0) session still reclaims its attached home on
    * destroy — cleanup runs before the active/alive early-returns. */
   assert(s.iso_home && strcmp(s.iso_home, home) == 0);
   cli_session_destroy(&s);

   assert(stat(home, &st) != 0);                    /* the whole home tree is gone */
   assert(s.iso_home == NULL);                      /* handle freed + cleared */
   assert(stat(creds, &st) == 0 && st.st_size > 0); /* unrelated persistent file untouched */

   /* Idempotent / safe on a session with no isolated home. */
   cli_session_t s2;
   memset(&s2, 0, sizeof(s2));
   cli_session_destroy(&s2);

   unlink(creds);
   unlink(json);
   rmdir(cdir);
   rmdir(shared);
   printf("  PASS: test_isolated_home_reclaimed_on_destroy\n");
}

static void test_native_home_has_no_credentials_and_partitions_state(void)
{
   char a[65], b[65];
   memset(a, 'a', 64);
   memset(b, 'b', 64);
   a[64] = b[64] = 0;
   const char *vendors[] = {"codex", "claude"};
   for (size_t i = 0; i < 2; i++)
   {
      char first[PATH_MAX], other[PATH_MAX], resumed[PATH_MAX], path[PATH_MAX];
      cli_session_t one = {0}, two = {0}, again = {0};
      assert(!cli_session_native_home(vendors[i], "/native/project", "native-source", a,
                                      i ? a : NULL, first, sizeof first, &one));
      if (i)
      {
         snprintf(path, sizeof path, "%s/.claude/.claude.json", first);
         FILE *config = fopen(path, "r");
         assert(config);
         char bytes[1024];
         size_t n = fread(bytes, 1, sizeof bytes - 1, config);
         assert(!ferror(config) && feof(config) && !fclose(config));
         bytes[n] = 0;
         cJSON *value = cJSON_Parse(bytes);
         const char *identity =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, "userID"));
         assert(identity && !strcmp(identity, a));
         cJSON_Delete(value);
         snprintf(path, sizeof path, "%s/.claude.json", first);
         assert(access(path, F_OK));
      }
      const char *state = i ? "projects" : "sessions";
      snprintf(path, sizeof path, "%s/.%s/%s/retained", first, vendors[i], state);
      FILE *file = fopen(path, "w");
      assert(file);
      fputs("owned native conversation", file);
      fclose(file);
      assert(!cli_session_native_home(vendors[i], "/native/project", "native-source", b,
                                      i ? b : NULL, other, sizeof other, &two));
      snprintf(path, sizeof path, "%s/.%s/%s/retained", other, vendors[i], state);
      assert(access(path, F_OK) != 0);
      snprintf(path, sizeof path, "%s/.%s/%s", first, vendors[i],
               i ? ".credentials.json" : "auth.json");
      assert(access(path, F_OK) != 0);
      snprintf(path, sizeof path, "%s/.%s/%s", first, vendors[i],
               i ? "settings.json" : "config.toml");
      assert(access(path, F_OK) != 0);
      cli_session_destroy(&one);
      assert(access(first, F_OK) != 0);
      assert(!cli_session_native_home(vendors[i], "/native/project", "native-source", a,
                                      i ? a : NULL, resumed, sizeof resumed, &again));
      snprintf(path, sizeof path, "%s/.%s/%s/retained", resumed, vendors[i], state);
      assert(access(path, F_OK) == 0);
      char *opaque = slurp(path);
      assert(opaque && !strcmp(opaque, "owned native conversation"));
      free(opaque);
      unlink(path);
      cli_session_destroy(&again);
      cli_session_destroy(&two);
      assert(cli_session_native_home(vendors[i], ".", "../other", a, i ? a : NULL, first,
                                     sizeof first, &one) < 0);
      assert(cli_session_native_home(vendors[i], ".", "native-source", "../other", i ? a : NULL,
                                     first, sizeof first, &one) < 0);
   }
   puts("  PASS: credential-free native homes retain only session/connection-owned state");
}

int main(void)
{
   install_fake_tmux();
   test_native_home_has_no_credentials_and_partitions_state();
   test_isolated_home_reclaimed_on_destroy();
   test_create_multiword_cli_cmd_single_arg();
   test_new_session_loads_bashrc();
   test_make_name_format();
   test_make_name_deterministic();
   test_make_name_different_roles();
   test_make_name_sanitizes_chars();
   test_make_name_null_role();
   puts("All retained CLI lifecycle/native-home tests passed.");
   return 0;
}
