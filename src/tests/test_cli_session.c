/* test_cli_session.c: unit tests for cli_session pure-C functions */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "cli_session.h"

/* --- cli_session_recv timeout tests ---
 *
 * recv() shells out to `tmux has-session` / `tmux capture-pane`. We stub tmux
 * with a fake script on PATH whose behaviour is selected by $FAKE_TMUX_MODE,
 * so the receive loop can be driven deterministically without a real tmux:
 *   changing → always alive, capture prints a new value each call (never
 *              stabilises) → exercises the wall-clock timeout backstop
 *   stable   → always alive, capture prints a constant → stabilises → OK
 *   dead     → has-session fails → session-died path
 */
static char g_fake_dir[256];

static long long test_mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void install_fake_tmux(void)
{
   snprintf(g_fake_dir, sizeof(g_fake_dir), "/tmp/aimee_faketmux_%d", (int)getpid());
   mkdir(g_fake_dir, 0700);
   char counter[300];
   snprintf(counter, sizeof(counter), "%s/counter", g_fake_dir);
   char script[320];
   snprintf(script, sizeof(script), "%s/tmux", g_fake_dir);
   FILE *f = fopen(script, "w");
   assert(f != NULL);
   fprintf(f,
           "#!/bin/sh\n"
           "case \"$1\" in\n"
           "  has-session) [ \"$FAKE_TMUX_MODE\" = dead ] && exit 1; exit 0 ;;\n"
           "  capture-pane)\n"
           "    if [ \"$FAKE_TMUX_MODE\" = changing ]; then\n"
           "      c=0; [ -f '%s' ] && c=$(cat '%s'); c=$((c+1)); echo \"$c\" > '%s';\n"
           "      echo \"frame $c\";\n"
           "    else echo 'STATIC OUTPUT'; fi; exit 0 ;;\n"
           "  *) exit 0 ;;\n"
           "esac\n",
           counter, counter, counter);
   fclose(f);
   assert(chmod(script, 0700) == 0);

   const char *old_path = getenv("PATH");
   char newpath[4096];
   snprintf(newpath, sizeof(newpath), "%s:%s", g_fake_dir, old_path ? old_path : "");
   setenv("PATH", newpath, 1);
}

static cli_session_t fake_session(void)
{
   cli_session_t s;
   memset(&s, 0, sizeof(s));
   s.active = 1;
   snprintf(s.session_name, sizeof(s.session_name), "aimee-faketest");
   return s;
}

static void test_recv_timeout_on_changing_pane(void)
{
   setenv("FAKE_TMUX_MODE", "changing", 1);
   cli_session_t s = fake_session();
   char buf[8192];
   long long t0 = test_mono_ms();
   int rc = cli_session_recv(&s, buf, sizeof(buf), 1000);
   long long elapsed = test_mono_ms() - t0;
   /* The pane never stabilises, so recv must hit the wall-clock bound (-2)
    * rather than hang. Allow generous slack above the 1000ms bound. */
   assert(rc == -2);
   assert(elapsed < 5000);
}

static void test_recv_ok_on_stable_pane(void)
{
   setenv("FAKE_TMUX_MODE", "stable", 1);
   cli_session_t s = fake_session();
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   assert(rc == 0);
   assert(strstr(buf, "STATIC OUTPUT") != NULL);
}

static void test_recv_dead_session(void)
{
   setenv("FAKE_TMUX_MODE", "dead", 1);
   cli_session_t s = fake_session();
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   assert(rc == -1);
}

/* --- cli_session_make_name tests --- */

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

/* --- cli_session_strip_ansi tests --- */

static void test_strip_ansi_plain_text(void)
{
   char *out = cli_session_strip_ansi("hello world");
   assert(out != NULL);
   assert(strcmp(out, "hello world") == 0);
   free(out);
}

static void test_strip_ansi_removes_escapes(void)
{
   /* Bold red text: \033[1;31mhello\033[0m */
   char *out = cli_session_strip_ansi("\033[1;31mhello\033[0m");
   assert(out != NULL);
   assert(strcmp(out, "hello") == 0);
   free(out);
}

static void test_strip_ansi_multiple_sequences(void)
{
   char *out = cli_session_strip_ansi("\033[32mgreen\033[0m \033[34mblue\033[0m");
   assert(out != NULL);
   assert(strcmp(out, "green blue") == 0);
   free(out);
}

static void test_strip_ansi_null_input(void)
{
   char *out = cli_session_strip_ansi(NULL);
   assert(out != NULL);
   assert(out[0] == '\0');
   free(out);
}

static void test_strip_ansi_empty_string(void)
{
   char *out = cli_session_strip_ansi("");
   assert(out != NULL);
   assert(out[0] == '\0');
   free(out);
}

static void test_strip_ansi_preserves_newlines(void)
{
   char *out = cli_session_strip_ansi("line1\nline2\n");
   assert(out != NULL);
   assert(strcmp(out, "line1\nline2\n") == 0);
   free(out);
}

static void test_strip_ansi_mixed(void)
{
   char *out = cli_session_strip_ansi("prefix \033[1mBOLD\033[0m suffix");
   assert(out != NULL);
   assert(strcmp(out, "prefix BOLD suffix") == 0);
   free(out);
}

static void test_strip_ansi_handles_trailing_escape(void)
{
   char *out = cli_session_strip_ansi("hello\033");
   assert(out != NULL);
   assert(strcmp(out, "hello\033") == 0);
   free(out);
}

/* --- cli_session_delta tests --- */

static void test_delta_appended_suffix(void)
{
   char *out = cli_session_delta("line1\n", "line1\nline2\n");
   assert(out != NULL);
   assert(strcmp(out, "line2\n") == 0);
   free(out);
}

static void test_delta_initial_snapshot(void)
{
   char *out = cli_session_delta("", "full output");
   assert(out != NULL);
   assert(strcmp(out, "full output") == 0);
   free(out);
}

static void test_delta_non_prefix_falls_back_to_full(void)
{
   char *out = cli_session_delta("old output", "rewrapped output");
   assert(out != NULL);
   assert(strcmp(out, "rewrapped output") == 0);
   free(out);
}

int main(void)
{
   printf("test_make_name_format... ");
   test_make_name_format();
   printf("OK\n");

   printf("test_make_name_deterministic... ");
   test_make_name_deterministic();
   printf("OK\n");

   printf("test_make_name_different_roles... ");
   test_make_name_different_roles();
   printf("OK\n");

   printf("test_make_name_sanitizes_chars... ");
   test_make_name_sanitizes_chars();
   printf("OK\n");

   printf("test_make_name_null_role... ");
   test_make_name_null_role();
   printf("OK\n");

   printf("test_strip_ansi_plain_text... ");
   test_strip_ansi_plain_text();
   printf("OK\n");

   printf("test_strip_ansi_removes_escapes... ");
   test_strip_ansi_removes_escapes();
   printf("OK\n");

   printf("test_strip_ansi_multiple_sequences... ");
   test_strip_ansi_multiple_sequences();
   printf("OK\n");

   printf("test_strip_ansi_null_input... ");
   test_strip_ansi_null_input();
   printf("OK\n");

   printf("test_strip_ansi_empty_string... ");
   test_strip_ansi_empty_string();
   printf("OK\n");

   printf("test_strip_ansi_preserves_newlines... ");
   test_strip_ansi_preserves_newlines();
   printf("OK\n");

   printf("test_strip_ansi_mixed... ");
   test_strip_ansi_mixed();
   printf("OK\n");

   printf("test_strip_ansi_handles_trailing_escape... ");
   test_strip_ansi_handles_trailing_escape();
   printf("OK\n");

   printf("test_delta_appended_suffix... ");
   test_delta_appended_suffix();
   printf("OK\n");

   printf("test_delta_initial_snapshot... ");
   test_delta_initial_snapshot();
   printf("OK\n");

   printf("test_delta_non_prefix_falls_back_to_full... ");
   test_delta_non_prefix_falls_back_to_full();
   printf("OK\n");

   install_fake_tmux();

   printf("test_recv_timeout_on_changing_pane... ");
   test_recv_timeout_on_changing_pane();
   printf("OK\n");

   printf("test_recv_ok_on_stable_pane... ");
   test_recv_ok_on_stable_pane();
   printf("OK\n");

   printf("test_recv_dead_session... ");
   test_recv_dead_session();
   printf("OK\n");

   printf("All cli_session tests passed.\n");
   return 0;
}
