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

static char g_claude_vault_oauth[512];
static char g_codex_vault_oauth[512];

int cli_oauth_vault_materialize_get(const char *agent, char *out, size_t out_len)
{
   const char *value = strcmp(agent, "claude") == 0  ? g_claude_vault_oauth
                       : strcmp(agent, "codex") == 0 ? g_codex_vault_oauth
                                                     : "";
   if (!value[0] || !out || out_len == 0)
      return 1;
   snprintf(out, out_len, "%s", value);
   return 0;
}

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

/* Path the fake tmux appends every `new-session` argv element to (one per
 * line, bracketed), so the create test can assert the pane command reached
 * tmux as a single intact argument rather than word-split fragments. */
static char g_createlog[300];

/* Path the fake tmux appends every `send-keys` invocation to, so a test can
 * assert which interrupt key (if any) the cancel path sent. */
static char g_sendlog[320];
static char g_capturelog[320];

static void install_fake_tmux(void)
{
   snprintf(g_fake_dir, sizeof(g_fake_dir), "%s/aimee_faketmux_%d", platform_tmpdir(),
            (int)getpid());
   mkdir(g_fake_dir, 0700);
   char counter[300];
   snprintf(counter, sizeof(counter), "%s/counter", g_fake_dir);
   snprintf(g_sendlog, sizeof(g_sendlog), "%s/sendlog", g_fake_dir);
   snprintf(g_capturelog, sizeof(g_capturelog), "%s/capturelog", g_fake_dir);
   snprintf(g_createlog, sizeof(g_createlog), "%s/createlog", g_fake_dir);
   char script[320];
   snprintf(script, sizeof(script), "%s/tmux", g_fake_dir);
   FILE *f = fopen(script, "w");
   assert(f != NULL);
   /* capture-pane modes: `changing` never stabilises; `codexgen` returns a codex
    * generating-footer; `provider_error` animates claude's ✻ error/retry status
    * line (never stabilises); `banner_retry` animates a │-prefixed box line whose
    * prose contains "Retrying in" (mimics the welcome banner — must NOT be read as
    * a provider error); `busy_tool` returns a STATIC pane whose footer still shows
    * "esc to interrupt" (a long-running tool — recv must NOT finalize it);
    * `prompt_then_answer` holds a pasted prompt static past the stability threshold
    * before rendering a Claude assistant bullet; anything else returns a static
    * pane. send-keys is logged so the cancel/error tests can
    * assert the interrupt key. */
   fprintf(f,
           "#!/bin/sh\n"
           "case \"$1\" in\n"
           "  has-session) [ \"$FAKE_TMUX_MODE\" = dead ] && exit 1; exit 0 ;;\n"
           "  capture-pane)\n"
           "    echo \"$*\" >> '%s';\n"
           "    if [ \"$FAKE_TMUX_MODE\" = changing ]; then\n"
           "      c=0; [ -f '%s' ] && c=$(cat '%s'); c=$((c+1)); echo \"$c\" > '%s';\n"
           "      echo \"frame $c\";\n"
           "    elif [ \"$FAKE_TMUX_MODE\" = codexgen ]; then\n"
           "      echo 'codex output'; echo 'Working (1s esc to interrupt)';\n"
           "    elif [ \"$FAKE_TMUX_MODE\" = busy_tool ]; then\n"
           "      printf '%%s\\n' '\xe2\x97\x8f Bash(sed -n 1,80p f)' '  \xe2\x8e\xbf Waiting'"
           " 'esc to interrupt';\n"
           "    elif [ \"$FAKE_TMUX_MODE\" = provider_error ]; then\n"
           "      c=0; [ -f '%s' ] && c=$(cat '%s'); c=$((c+1)); echo \"$c\" > '%s';\n"
           "      echo '\xe2\x9c\xbb API error Retrying in 0s attempt '\"$c\"'/10';\n"
           "    elif [ \"$FAKE_TMUX_MODE\" = banner_retry ]; then\n"
           "      c=0; [ -f '%s' ] && c=$(cat '%s'); c=$((c+1)); echo \"$c\" > '%s';\n"
           "      echo '\xe2\x94\x82 stream-stall hint now reads Retrying in seconds frame "
           "'\"$c\"' end';\n"
           "    elif [ \"$FAKE_TMUX_MODE\" = prompt_then_answer ]; then\n"
           "      c=0; [ -f '%s' ] && c=$(cat '%s'); c=$((c+1)); echo \"$c\" > '%s';\n"
           "      if [ \"$c\" -le 6 ]; then echo '\xe2\x9d\xaf pasted repair prompt'; "
           "else printf '%%s\\n' '\xe2\x9d\xaf pasted repair prompt' "
           "'\xe2\x97\x8f {\"ok\":true}' '\xe2\x9c\xbb Baked for 1s'; fi;\n"
           "    else echo 'STATIC OUTPUT'; fi; exit 0 ;;\n"
           "  send-keys) shift; echo \"$*\" >> '%s'; exit 0 ;;\n"
           "  new-session) shift; for a in \"$@\"; do echo \"ARG:[$a]\" >> '%s'; done; exit 0 ;;\n"
           "  *) exit 0 ;;\n"
           "esac\n",
           g_capturelog, counter, counter, counter, counter, counter, counter, counter, counter,
           counter, counter, counter, counter, g_sendlog, g_createlog);
   fclose(f);
   assert(chmod(script, 0700) == 0);

   const char *old_path = getenv("PATH");
   char newpath[4096];
   snprintf(newpath, sizeof(newpath), "%s:%s", g_fake_dir, old_path ? old_path : "");
   setenv("PATH", newpath, 1);
}

/* Cancel-check fixture: returns the value of *flag (an int the test toggles). */
static int g_test_cancel_flag;
static int test_cancel_cb(void *ud)
{
   (void)ud;
   return g_test_cancel_flag;
}
/* True if the fake tmux send-keys log contains `needle`. */
static int sendlog_has(const char *needle)
{
   FILE *f = fopen(g_sendlog, "r");
   if (!f)
      return 0;
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return strstr(buf, needle) != NULL;
}

/* True if the fake tmux new-session argv log contains `needle`. */
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

static int capturelog_has(const char *needle)
{
   FILE *f = fopen(g_capturelog, "r");
   if (!f)
      return 0;
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return strstr(buf, needle) != NULL;
}

/* cli_session_create must hand tmux the pane command (and workdir) as ONE
 * argument each. cli_cmd is multi-word in production (the AIMEE_SESSION_ID
 * stamp, --model, --dangerously-skip-permissions): unquoted interpolation gets
 * word-split by the outer shell, tmux re-joins the fragments, and the pane's
 * `sh -c` executes only the leading env assignment — exiting 0 instantly and
 * surfacing as "failed to send prompt to tmux session". */
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
   assert(createlog_has("ARG:[AIMEE_SESSION_ID=web-1 claude --dangerously-skip-permissions]"));
   char expect_wd[300];
   snprintf(expect_wd, sizeof expect_wd, "ARG:[%s]", quoting_wd);
   assert(createlog_has(expect_wd));
   /* No fragment may arrive as its own argument (the word-split regression). */
   assert(!createlog_has("ARG:[AIMEE_SESSION_ID=web-1]"));
   s.active = 0; /* fake tmux: no real session to tear down */
}
static void sendlog_reset(void)
{
   unlink(g_sendlog);
}

static cli_session_t fake_session(void)
{
   cli_session_t s;
   memset(&s, 0, sizeof(s));
   s.active = 1;
   snprintf(s.session_name, sizeof(s.session_name), "aimee-faketest");
   return s;
}

/* The wall-clock ceiling the recv tests below assert against.
 *
 * These ceilings answer "did recv terminate on its own bound, or run away?" —
 * they are not performance targets, and the behaviour is already asserted
 * separately by the return code above each one.
 *
 * It was 5000ms, which is 5x the 1000ms bound the first two tests pass in and
 * 6x the 800ms grace the third one sets. That margin does not survive a busy
 * machine: under the parallel suite this file failed 11 runs in 12, always on
 * the clock and never on the behaviour — in the captured failure `rc == -4` and
 * the `sendlog_has("Escape")` assertion both passed, and only `elapsed < 5000`
 * did not.
 *
 * A ceiling only has to sit far enough below the WRONG outcome to tell the two
 * apart. For the idle-timeout tests the wrong outcome is "never returns"; for
 * the grace test it is riding the 30s idle bound instead of the 800ms grace.
 * 15s is comfortably below both while giving scheduling ~15-18x the budget the
 * code actually needs, so a correct run cannot lose the race and an incorrect
 * one still cannot pass. */
#define RECV_TERMINATION_CEILING_MS 15000

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
   assert(elapsed < RECV_TERMINATION_CEILING_MS);
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

static int g_hb_calls;
static void hb_counter_cb(void *ud)
{
   (void)ud;
   g_hb_calls++;
}

/* A tmux-CLI turn makes no aimee HTTP calls, so recv must drive the liveness
 * heartbeat itself or the stale-idle monitor reaps a healthy long turn (the
 * claude roundtable-seat regression). The heartbeat fires on the first loop
 * iteration, so even a short recv proves the wiring. */
static void test_recv_drives_heartbeat(void)
{
   setenv("FAKE_TMUX_MODE", "changing", 1);
   cli_session_t s = fake_session();
   char buf[8192];
   g_hb_calls = 0;
   cli_session_set_heartbeat_cb(hb_counter_cb, NULL);
   (void)cli_session_recv(&s, buf, sizeof(buf), 1000);
   cli_session_set_heartbeat_cb(NULL, NULL);
   assert(g_hb_calls >= 1);

   /* With no callback set, recv must not crash and must not call the old one. */
   g_hb_calls = 0;
   s = fake_session();
   (void)cli_session_recv(&s, buf, sizeof(buf), 1000);
   assert(g_hb_calls == 0);
}

static void test_capture_joins_wraps_and_includes_scrollback(void)
{
   unlink(g_capturelog);
   setenv("FAKE_TMUX_MODE", "stable", 1);
   cli_session_t s = fake_session();
   char buf[8192];
   assert(cli_session_capture(&s, buf, sizeof(buf)) == 0);
   assert(capturelog_has("capture-pane -p -J -S - -t aimee-faketest"));
}

static void test_recv_does_not_return_static_prompt_as_answer(void)
{
   setenv("FAKE_TMUX_MODE", "prompt_then_answer", 1);
   char counter[300];
   snprintf(counter, sizeof(counter), "%s/counter", g_fake_dir);
   unlink(counter);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "claude");
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   assert(rc == 0);
   assert(strcmp(buf, "{\"ok\":true}") == 0);
}

static void test_recv_dead_session(void)
{
   setenv("FAKE_TMUX_MODE", "dead", 1);
   cli_session_t s = fake_session();
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   assert(rc == -1);
}

/* A pane that is STATIC but whose footer still says "esc to interrupt" is a turn
 * mid-tool (a long-running Bash), not a finished one: recv must NOT finalize it
 * (which would ship the half-rendered tool call and freeze the webchat). With no
 * completion it rides the wall-clock bound to -2 instead of a premature rc 0. */
static void test_recv_busy_footer_not_finalized(void)
{
   setenv("FAKE_TMUX_MODE", "busy_tool", 1);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "claude");
   char buf[8192];
   long long t0 = test_mono_ms();
   int rc = cli_session_recv(&s, buf, sizeof(buf), 1000);
   long long elapsed = test_mono_ms() - t0;
   assert(rc == -2); /* never finalized; hit the wall-clock bound */
   assert(elapsed < RECV_TERMINATION_CEILING_MS);
}

/* --- cancel / steering-interrupt path --- */

/* claude: a fired cancel-check returns -3 and sends Escape, promptly (no need to
 * wait for the pane to stabilise). */
static void test_recv_cancel_claude_escape(void)
{
   setenv("FAKE_TMUX_MODE", "stable", 1);
   sendlog_reset();
   g_test_cancel_flag = 1;
   cli_session_set_cancel_check(test_cancel_cb, NULL);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "claude");
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   cli_session_set_cancel_check(NULL, NULL);
   g_test_cancel_flag = 0;
   assert(rc == -3);
   assert(sendlog_has("Escape"));
   assert(!sendlog_has("C-c"));
}

/* codex while generating (footer shows the interrupt hint): cancel sends C-c. */
static void test_recv_cancel_codex_generating_ctrlc(void)
{
   setenv("FAKE_TMUX_MODE", "codexgen", 1);
   sendlog_reset();
   g_test_cancel_flag = 1;
   cli_session_set_cancel_check(test_cancel_cb, NULL);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "codex");
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   cli_session_set_cancel_check(NULL, NULL);
   g_test_cancel_flag = 0;
   assert(rc == -3);
   assert(sendlog_has("C-c"));
}

/* codex while idle (no generating footer): cancel still returns -3 but must NOT
 * send C-c — an idle C-c would quit codex. */
static void test_recv_cancel_codex_idle_skips_ctrlc(void)
{
   setenv("FAKE_TMUX_MODE", "stable", 1); /* capture returns "STATIC OUTPUT" — no hint */
   sendlog_reset();
   g_test_cancel_flag = 1;
   cli_session_set_cancel_check(test_cancel_cb, NULL);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "codex");
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 10000);
   cli_session_set_cancel_check(NULL, NULL);
   g_test_cancel_flag = 0;
   assert(rc == -3);
   assert(!sendlog_has("C-c"));
}

/* --- provider-error grace path --- */

/* claude parked in its ✻ error/retry status past the grace: recv returns -4,
 * stops the retry with Escape, and is bounded by the grace (not the idle
 * timeout). */
static void test_recv_provider_error_returns_minus4(void)
{
   setenv("FAKE_TMUX_MODE", "provider_error", 1);
   sendlog_reset();
   cli_session_set_error_grace_ms(800);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "claude");
   char buf[8192];
   long long t0 = test_mono_ms();
   int rc = cli_session_recv(&s, buf, sizeof(buf), 30000); /* idle bound high; grace fires first */
   long long elapsed = test_mono_ms() - t0;
   cli_session_set_error_grace_ms(0);
   assert(rc == -4);
   assert(sendlog_has("Escape")); /* stopped the retry loop */
   /* bounded by the 800ms grace, not the 30s idle timeout */
   assert(elapsed < RECV_TERMINATION_CEILING_MS);
}

/* grace = 0 (default/opt-in off): the error pane is just a non-stabilising pane,
 * so recv falls through to the idle timeout (-2) — legacy behaviour preserved. */
static void test_recv_provider_error_disabled_times_out(void)
{
   setenv("FAKE_TMUX_MODE", "provider_error", 1);
   cli_session_set_error_grace_ms(0);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "claude");
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 1000);
   assert(rc == -2);
}

/* The welcome banner's "Retrying in …" prose (│-prefixed box line, no ✻ status
 * star) must NOT be read as a provider error: with the grace set, recv still
 * hits the idle timeout (-2), never -4. Guards the anchoring against a false
 * positive on banner text. */
static void test_recv_banner_retry_not_provider_error(void)
{
   setenv("FAKE_TMUX_MODE", "banner_retry", 1);
   cli_session_set_error_grace_ms(800);
   cli_session_t s = fake_session();
   cli_session_set_kind(&s, "claude");
   char buf[8192];
   int rc = cli_session_recv(&s, buf, sizeof(buf), 1500);
   cli_session_set_error_grace_ms(0);
   assert(rc == -2);
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

static void test_execution_name_isolates_override_less_delegates(void)
{
   int reuse_a = 1, reuse_b = 1;
   char *a =
       cli_session_make_execution_name("claude", 1, "process-fallback", 0, "deleg-a", 0, &reuse_a);
   char *b =
       cli_session_make_execution_name("claude", 1, "process-fallback", 0, "deleg-b", 0, &reuse_b);
   char *again = cli_session_make_execution_name("claude", 1, NULL, 0, "deleg-a", 0, NULL);
   assert(a && b && again);
   assert(strcmp(a, b) != 0);
   assert(strcmp(a, again) == 0);
   assert(reuse_a == 0 && reuse_b == 0);
   free(a);
   free(b);
   free(again);
}

static void test_execution_name_preserves_primary_reuse(void)
{
   int reuse = 0;
   char *bound = cli_session_make_execution_name("claude", 0, "web-123", 1, NULL, 0, &reuse);
   assert(bound && reuse == 1);
   char *want = cli_session_make_name("web-123", "cli");
   assert(want && strcmp(bound, want) == 0);
   free(bound);
   free(want);

   char *shared = cli_session_make_execution_name("claude", 1, NULL, 0, NULL, 0, &reuse);
   assert(shared && reuse == 1);
   want = cli_session_make_name("claude", "shared");
   assert(want && strcmp(shared, want) == 0);
   free(shared);
   free(want);

   char *isolated = cli_session_make_execution_name("claude", 1, "web-123", 1, NULL, 1, &reuse);
   char *isolated_again = cli_session_make_execution_name("claude", 1, "web-123", 1, NULL, 1, NULL);
   assert(isolated && isolated_again && reuse == 0);
   assert(strcmp(isolated, isolated_again) != 0);
   free(isolated);
   free(isolated_again);
}

/* --- cli_session_resolve_cwd tests --- */

/* A bound cwd that exists on this host is used verbatim; no fallback. */
static void test_resolve_cwd_existing_used(void)
{
   char out[256];
   int fb = cli_session_resolve_cwd("/tmp", out, sizeof(out));
   assert(fb == 0);
   assert(strcmp(out, "/tmp") == 0);
}

/* A non-empty bound cwd that is ABSENT on this host (the detached thin-client
 * case that used to make every tmux shell-out `cd` fail) falls back to the
 * server process cwd and reports the fallback so the caller retargets run_cmd. */
static void test_resolve_cwd_missing_falls_back(void)
{
   char out[4096], here[4096];
   assert(getcwd(here, sizeof(here)) != NULL);
   int fb = cli_session_resolve_cwd("/no/such/dir/aimee-does-not-exist-xyz", out, sizeof(out));
   assert(fb == 1);
   assert(strcmp(out, here) == 0);
}

/* An empty / NULL bound cwd is the ordinary co-located case: use the server cwd
 * but report NO fallback (run_cmd has no `cd` prefix to retarget). */
static void test_resolve_cwd_empty_no_fallback(void)
{
   char out[4096], here[4096];
   assert(getcwd(here, sizeof(here)) != NULL);
   int fb = cli_session_resolve_cwd(NULL, out, sizeof(out));
   assert(fb == 0);
   assert(strcmp(out, here) == 0);
   fb = cli_session_resolve_cwd("", out, sizeof(out));
   assert(fb == 0);
   assert(strcmp(out, here) == 0);
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

/* --- response extraction (TUI scrape) --------------------------------------
 * Markers: claude assistant ●(\xe2\x97\x8f) user ❯(\xe2\x9d\xaf) status ✻(\xe2\x9c\xbb);
 * codex assistant •(\xe2\x80\xa2) user ›(\xe2\x80\xba). Captures mirror the real panes. */

static void test_extract_claude_basic(void)
{
   const char *pane = "\xe2\x9d\xaf Reply with three words\n"
                      "\xe2\x97\x8f alpha bravo charlie\n"
                      "\xe2\x9c\xbb Cooked for 1s\n"
                      "\n"
                      "\xe2\x9d\xaf \n"
                      "  ? for shortcuts\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "alpha bravo charlie") == 0);
   free(r);
}

/* Reused pane holding a prior turn: with the prior turn as baseline, only the
 * NEW turn's reply is returned — the core anti-bleed guarantee. */
static void test_extract_claude_excludes_prior_turn(void)
{
   const char *baseline = "\xe2\x9d\xaf first question\n"
                          "\xe2\x97\x8f first answer here\n"
                          "\xe2\x9c\xbb Cooked for 1s\n";
   const char *pane = "\xe2\x9d\xaf first question\n"
                      "\xe2\x97\x8f first answer here\n"
                      "\xe2\x9c\xbb Cooked for 1s\n"
                      "\xe2\x9d\xaf second question\n"
                      "\xe2\x97\x8f second answer only\n"
                      "\xe2\x9c\xbb Baked for 2s\n"
                      "\xe2\x9d\xaf \n";
   char *r = cli_session_extract_response(pane, "claude", baseline);
   assert(r != NULL);
   assert(strcmp(r, "second answer only") == 0);
   free(r);
}

static void test_extract_claude_multiline(void)
{
   const char *pane = "\xe2\x9d\xaf q\n"
                      "\xe2\x97\x8f line one\n"
                      "  line two\n"
                      "\xe2\x9c\xbb Cooked for 1s\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "line one\nline two") == 0);
   free(r);
}

/* Claude renders long JSON strings as hard terminal rows.  tmux marks those as
 * real newlines (not soft wraps), so extraction must make the structured result
 * valid without flattening pretty-printed newlines between JSON fields. */
static void test_extract_claude_json_hard_wrap_inside_string(void)
{
   const char *pane = "\xe2\x9d\xaf q\n"
                      "\xe2\x97\x8f {\"status\":\"ok\",\"evidence\":\"long text at the hard\n"
                      "  terminal wrap\",\n"
                      "  \"findings\":[]}\n"
                      "\xe2\x9c\xbb Cooked for 1s\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   cJSON *parsed = cJSON_Parse(r);
   assert(parsed != NULL);
   cJSON *evidence = cJSON_GetObjectItemCaseSensitive(parsed, "evidence");
   assert(cJSON_IsString(evidence));
   assert(strstr(evidence->valuestring, "hard terminal wrap") != NULL);
   cJSON_Delete(parsed);
   free(r);
}

/* Regression (found by live e2e): claude renders its model/effort status with the
 * SAME ● bullet as a real answer ("● high · /effort"); it must be treated as
 * chrome and skipped, not returned as the reply. */
static void test_extract_claude_skips_effort_status(void)
{
   const char *pane = "\xe2\x9d\xaf q\n"
                      "\xe2\x97\x8f high \xc2\xb7 /effort\n" /* ● high · /effort — status */
                      "\xe2\x97\x8f PINEAPPLE\n"
                      "\xe2\x9c\xbb Cooked for 1s\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "PINEAPPLE") == 0);
   free(r);
}

/* The /effort chrome match is anchored to the "· /effort" status format, so a
 * legitimate answer that merely mentions /effort is NOT skipped. */
static void test_extract_claude_effort_in_answer_kept(void)
{
   const char *pane = "\xe2\x9d\xaf q\n"
                      "\xe2\x97\x8f Use the /effort command to set the depth.\n"
                      "\xe2\x9c\xbb Cooked for 1s\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "Use the /effort command to set the depth.") == 0);
   free(r);
}

static void test_extract_codex_basic(void)
{
   /* codex events also use •; the SessionStart hook fired before the turn, so it
    * is in the baseline (captured pre-send) and excluded — the answer is the new
    * bullet. */
   const char *baseline = "\xe2\x80\xa2 SessionStart hook (completed)\n"
                          "  hook context noise\n";
   const char *pane = "\xe2\x80\xba Reply with three words\n"
                      "\xe2\x80\xa2 SessionStart hook (completed)\n"
                      "  hook context noise\n"
                      "\xe2\x80\xa2 foxtrot golf hotel\n"
                      "\xe2\x80\xba Find and fix a bug\n"
                      "  gpt-5.5 default\n";
   char *r = cli_session_extract_response(pane, "codex", baseline);
   assert(r != NULL);
   assert(strcmp(r, "foxtrot golf hotel") == 0);
   free(r);
}

/* Multi-bullet answer: all bullets of the turn are kept, not just the last. */
static void test_extract_claude_multibullet(void)
{
   const char *pane = "\xe2\x9d\xaf q\n"
                      "\xe2\x97\x8f paragraph one\n"
                      "\xe2\x97\x8f paragraph two\n"
                      "\xe2\x9c\xbb Cooked for 1s\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "paragraph one\nparagraph two") == 0);
   free(r);
}

/* A short reply that merely appears as a SUBSTRING of a prior line must NOT be
 * excluded — the baseline match is whole-line, not substring. */
static void test_extract_baseline_substring_kept(void)
{
   const char *baseline = "\xe2\x97\x8f that looks ok to me\n"
                          "\xe2\x9c\xbb Cooked for 1s\n";
   const char *pane = "\xe2\x97\x8f that looks ok to me\n"
                      "\xe2\x9c\xbb Cooked for 1s\n"
                      "\xe2\x9d\xaf next\n"
                      "\xe2\x97\x8f ok\n"
                      "\xe2\x9c\xbb Baked for 1s\n";
   char *r = cli_session_extract_response(pane, "claude", baseline);
   assert(r != NULL);
   assert(strcmp(r, "ok") == 0);
   free(r);
}

/* --- spinner/working line is chrome, never answer text --- */

/* claude cycles the spinner glyph + gerund every frame (✻ ✽ ✢ · …, Misting /
 * Channeling / …). The footer the capture happens to catch must NOT leak into the
 * extracted answer — anchoring on the ✻ glyph alone missed the other frames and
 * spammed the transcript. Here the footer carries the ✽ frame: the answer is
 * still just the bullet text. */
static void test_extract_excludes_gerund_spinner(void)
{
   const char *pane = "\xe2\x9d\xaf write a poem\n"
                      "\xe2\x97\x8f Here is the poem\n"
                      "\xe2\x9c\xbd Channeling\xe2\x80\xa6 (14s \xc2\xb7 \xe2\x86\x91 823 tokens "
                      "\xc2\xb7 thinking)\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "Here is the poem") == 0);
   free(r);
}

/* The "·"-glyph frame (U+00B7, no leading star at all) is also a working line and
 * must be excluded — plus the "⎿ Tip" hint that trails it. */
static void test_extract_excludes_middot_spinner_and_tip(void)
{
   const char *pane =
       "\xe2\x9d\xaf q\n"
       "\xe2\x97\x8f real answer\n"
       "\xc2\xb7 Misting\xe2\x80\xa6 (31s \xc2\xb7 \xe2\x86\x93 2.5k tokens \xc2\xb7 thinking)\n"
       "\xe2\x8e\xbf Tip: Use /btw to ask a quick side question\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "real answer") == 0);
   free(r);
}

/* The "esc to interrupt" footer (the first ~30s before the gerund spinner) was
 * already excluded; keep that covered so the fix doesn't regress it. */
static void test_extract_excludes_interrupt_footer(void)
{
   const char *pane = "\xe2\x9d\xaf q\n"
                      "\xe2\x97\x8f answer body\n"
                      "  esc to interrupt\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(strcmp(r, "answer body") == 0);
   free(r);
}

/* The fresh-session welcome box (box-drawing + composer + footer, no answer
 * bullet) must NEVER be returned as the reply — the no-bullet fallback
 * noise-filters it to empty. Reproduces the live "webchat sends the Claude
 * banner" bug on the first turn before claude has processed the prompt. */
static void test_extract_welcome_banner_empty(void)
{
   const char *pane =
       "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80 Claude Code v2.1.186 "
       "\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae\n"
       "\xe2\x94\x82 Welcome back Jared!                  \xe2\x94\x82\n"
       "\xe2\x94\x82 Run /init to create a CLAUDE.md file  \xe2\x94\x82\n"
       "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf\n"
       "\xe2\x9d\xaf Try \"edit serverhttproutes.inc to...\"\n"
       "gh auth login \xc2\xb7 \xe2\x86\x90 for agents \xe2\x97\x8f high \xc2\xb7 /effort\n";
   char *r = cli_session_extract_response(pane, "claude", NULL);
   assert(r != NULL);
   assert(r[0] == '\0');
   free(r);
}

/* A real answer whose ● bullet has scrolled off the top of the pane still
 * survives the fallback (its body is plain text, not chrome) — the noise filter
 * must not over-strip. */
static void test_extract_scrolled_answer_survives_fallback(void)
{
   const char *baseline = "\xe2\x95\xad\xe2\x94\x80 Claude Code \xe2\x94\x80\xe2\x95\xae\n"
                          "\xe2\x9d\xaf Try something\n";
   const char *pane = "the second half of a long answer\n"
                      "that wrapped past the top of the pane\n"
                      "\xe2\x9c\xbb Baked for 4s\n";
   char *r = cli_session_extract_response(pane, "claude", baseline);
   assert(r != NULL);
   assert(strcmp(r, "the second half of a long answer\n"
                    "that wrapped past the top of the pane") == 0);
   free(r);
}

/* --- cli_session_prepare_claude: claude-code first-run gate seeding --- */

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

static void test_prepare_claude_seeds_gates(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee_clitest_XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   setenv("HOME", home, 1); /* cli_claude_home() resolves HOME first */
   setenv("AIMEE_HOME", home, 1);
   char wt_buf[256];
   snprintf(wt_buf, sizeof wt_buf, "%s/aimee_clitest_wt/session-abc", platform_tmpdir());
   const char *wt = wt_buf;

   cli_session_prepare_claude(wt, 1);

   char p[512];
   snprintf(p, sizeof(p), "%s/.claude.json", home);
   char *j = slurp(p);
   assert(j != NULL);
   cJSON *root = cJSON_Parse(j);
   free(j);
   assert(root != NULL);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hasCompletedOnboarding")));
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   assert(cJSON_IsObject(projects));
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, wt);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON_Delete(root);

   snprintf(p, sizeof(p), "%s/.claude/settings.json", home);
   char *s = slurp(p);
   assert(s != NULL);
   cJSON *sroot = cJSON_Parse(s);
   free(s);
   assert(sroot != NULL);
   assert(
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sroot, "skipDangerousModePermissionPrompt")));
   cJSON_Delete(sroot);

   /* Idempotent: a second call leaves the same state (and must not throw). */
   cli_session_prepare_claude(wt, 1);
   snprintf(p, sizeof(p), "%s/.claude.json", home);
   j = slurp(p);
   root = cJSON_Parse(j);
   free(j);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hasCompletedOnboarding")));
   cJSON_Delete(root);
}

static void test_prepare_claude_preserves_existing_settings(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee_clitest_XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   setenv("HOME", home, 1); /* cli_claude_home() resolves HOME first */
   setenv("AIMEE_HOME", home, 1);

   char dir[512], p[600];
   snprintf(dir, sizeof(dir), "%s/.claude", home);
   assert(mkdir(dir, 0700) == 0);
   snprintf(p, sizeof(p), "%s/settings.json", dir);
   FILE *f = fopen(p, "wb");
   assert(f != NULL);
   fputs("{\"theme\":\"dark\"}", f);
   fclose(f);

   char wt2[256];
   snprintf(wt2, sizeof wt2, "%s/aimee_clitest_wt2", platform_tmpdir());
   cli_session_prepare_claude(wt2, 1);

   char *s = slurp(p);
   cJSON *sroot = cJSON_Parse(s);
   free(s);
   assert(sroot != NULL);
   /* pre-existing key kept, new acceptance added */
   cJSON *theme = cJSON_GetObjectItemCaseSensitive(sroot, "theme");
   assert(cJSON_IsString(theme) && strcmp(theme->valuestring, "dark") == 0);
   assert(
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sroot, "skipDangerousModePermissionPrompt")));
   cJSON_Delete(sroot);
}

/* B1 regression: a config file that EXISTS but won't parse (claude-code writes
 * ~/.claude.json non-atomically, so a concurrent read can catch it mid-write)
 * must NOT be clobbered with a fresh {} — that would wipe the oauth account,
 * trust map, and history. prepare must leave it byte-for-byte untouched. */
static void test_prepare_claude_skips_unparseable_config(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee_clitest_XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   setenv("HOME", home, 1);
   setenv("AIMEE_HOME", home, 1);

   const char *corrupt = "{\"oauthAccount\":{\"emailAddress\":\"u@x\"},\"projects\":{ truncated";
   char jp[600];
   snprintf(jp, sizeof(jp), "%s/.claude.json", home);
   FILE *f = fopen(jp, "wb");
   assert(f != NULL);
   fputs(corrupt, f);
   fclose(f);

   char wt3[256];
   snprintf(wt3, sizeof wt3, "%s/aimee_clitest_wt3", platform_tmpdir());
   cli_session_prepare_claude(wt3, 1);

   char *after = slurp(jp);
   assert(after != NULL);
   assert(strcmp(after, corrupt) == 0); /* untouched, not wiped to {} */
   free(after);
}

/* Non-autonomous: onboarding + trust ARE seeded (the TUI needs them to start),
 * but the --dangerously-skip-permissions warning is NOT pre-accepted (the flag
 * isn't passed, so the operator's dangerous-mode prompt is left untouched). */
static void test_prepare_claude_nonautonomous_skips_bypass_seed(void)
{
   char home[256];
   snprintf(home, sizeof home, "%s/aimee_clitest_XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   setenv("HOME", home, 1);
   setenv("AIMEE_HOME", home, 1);
   char wt_buf[256];
   snprintf(wt_buf, sizeof wt_buf, "%s/aimee_clitest_wt4", platform_tmpdir());
   const char *wt = wt_buf;

   cli_session_prepare_claude(wt, 0); /* not autonomous */

   /* onboarding + trust seeded */
   char p[600];
   snprintf(p, sizeof(p), "%s/.claude.json", home);
   char *j = slurp(p);
   assert(j != NULL);
   cJSON *root = cJSON_Parse(j);
   free(j);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hasCompletedOnboarding")));
   cJSON *proj =
       cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "projects"), wt);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON_Delete(root);

   /* bypass warning NOT pre-accepted: settings.json absent or flag unset */
   snprintf(p, sizeof(p), "%s/.claude/settings.json", home);
   char *s = slurp(p);
   if (s)
   {
      cJSON *sroot = cJSON_Parse(s);
      free(s);
      assert(sroot == NULL || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                  sroot, "skipDangerousModePermissionPrompt")));
      cJSON_Delete(sroot);
   }
}

/* Per-session claude HOME isolation: the Vault credential is materialized as a
 * regular 0600 file beneath the runtime tier, never linked from persistent HOME. */
static void test_isolated_claude_home(void)
{
   char shared[128];
   snprintf(shared, sizeof(shared), "%s/aimee-iso-%d", platform_tmpdir(), (int)getpid());
   char runtime[160], cdir[192], json[288], settings[288];
   snprintf(runtime, sizeof(runtime), "%s/aimee-iso-runtime-%d", platform_tmpdir(), (int)getpid());
   snprintf(cdir, sizeof(cdir), "%s/.claude", shared);
   mkdir(shared, 0700);
   mkdir(cdir, 0700);
   snprintf(json, sizeof(json), "%s/.claude.json", shared);
   snprintf(settings, sizeof(settings), "%s/settings.json", cdir);
   FILE *f = fopen(json, "w");
   assert(f);
   fputs("{\"oauthAccount\":{\"id\":\"a\"}}", f);
   fclose(f);
   f = fopen(settings, "w");
   assert(f);
   fputs("{}", f);
   fclose(f);
   setenv("HOME", shared, 1);
   setenv("AIMEE_OAUTH_RUNTIME_DIR", runtime, 1);
   snprintf(g_claude_vault_oauth, sizeof(g_claude_vault_oauth), "{\"token\":\"vault-x\"}");

   char home[PATH_MAX];
   assert(cli_session_isolated_claude_home("/w/d", home, sizeof(home)) == 0);
   assert(strncmp(home, runtime, strlen(runtime)) == 0);

   struct stat st;
   char p[512];
   snprintf(p, sizeof(p), "%s/.claude/.credentials.json", home);
   assert(lstat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
   char *credential = slurp(p);
   assert(credential && strstr(credential, "vault-x"));
   free(credential);
   snprintf(p, sizeof(p), "%s/.claude.json", home);
   assert(stat(p, &st) == 0 && st.st_size > 0); /* per-session json seeded */
   FILE *rf = fopen(p, "r");
   assert(rf);
   char buf[4096] = {0};
   fread(buf, 1, sizeof(buf) - 1, rf);
   fclose(rf);
   assert(strstr(buf, "oauthAccount") && strstr(buf, "hasCompletedOnboarding"));
   assert(strstr(buf, "/w/d")); /* trust seeded for the worktree */

   /* No credential in Vault -> fail closed rather than fall back to HOME. */
   g_claude_vault_oauth[0] = '\0';
   char home2[PATH_MAX];
   assert(cli_session_isolated_claude_home("/w/d", home2, sizeof(home2)) == -1);
   printf("  PASS: test_isolated_claude_home\n");
}

/* cli_session_destroy reclaims the runtime-only HOME so homes track the delegate
 * lifecycle instead of lingering until the 1h age sweep. */
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
   snprintf(g_claude_vault_oauth, sizeof(g_claude_vault_oauth), "{\"token\":\"vault-y\"}");

   char home[PATH_MAX];
   assert(cli_session_isolated_claude_home("/w/d", home, sizeof(home)) == 0);
   struct stat st;
   assert(stat(home, &st) == 0 && S_ISDIR(st.st_mode)); /* minted on disk */

   /* A never-created (active=0) session still reclaims its attached home on
    * destroy — cleanup runs before the active/alive early-returns. */
   cli_session_t s;
   memset(&s, 0, sizeof(s));
   cli_session_set_isolated_home(&s, home);
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

static void test_isolated_codex_home(void)
{
   char shared[160], runtime[180], codex_dir[220], config[280];
   snprintf(shared, sizeof(shared), "%s/aimee-codex-shared-%d", platform_tmpdir(), (int)getpid());
   snprintf(runtime, sizeof(runtime), "%s/aimee-codex-runtime-%d", platform_tmpdir(),
            (int)getpid());
   snprintf(codex_dir, sizeof(codex_dir), "%s/.codex", shared);
   assert(mkdir(shared, 0700) == 0);
   assert(mkdir(codex_dir, 0700) == 0);
   snprintf(config, sizeof(config), "%s/config.toml", codex_dir);
   FILE *f = fopen(config, "wb");
   assert(f != NULL);
   fputs("model = \"test\"\n", f);
   fclose(f);
   setenv("HOME", shared, 1);
   setenv("AIMEE_OAUTH_RUNTIME_DIR", runtime, 1);
   snprintf(g_codex_vault_oauth, sizeof(g_codex_vault_oauth),
            "{\"tokens\":{\"access_token\":\"vault-codex\"}}");

   char home[PATH_MAX], path[PATH_MAX];
   assert(cli_session_isolated_codex_home(home, sizeof(home)) == 0);
   assert(strncmp(home, runtime, strlen(runtime)) == 0);
   struct stat st;
   snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
   assert(lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
   snprintf(path, sizeof(path), "%s/.codex/config.toml", home);
   assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));

   cli_session_t cleanup = {0};
   cli_session_set_isolated_home(&cleanup, home);
   cli_session_destroy(&cleanup);
   assert(access(home, F_OK) != 0);

   g_codex_vault_oauth[0] = '\0';
   assert(cli_session_isolated_codex_home(home, sizeof(home)) == -1);
   unlink(config);
   rmdir(codex_dir);
   rmdir(shared);
   printf("  PASS: test_isolated_codex_home\n");
}

int main(void)
{
   test_isolated_claude_home();
   test_isolated_home_reclaimed_on_destroy();
   test_isolated_codex_home();

   printf("test_resolve_cwd_existing_used... ");
   test_resolve_cwd_existing_used();
   printf("OK\n");

   printf("test_resolve_cwd_missing_falls_back... ");
   test_resolve_cwd_missing_falls_back();
   printf("OK\n");

   printf("test_resolve_cwd_empty_no_fallback... ");
   test_resolve_cwd_empty_no_fallback();
   printf("OK\n");

   printf("test_prepare_claude_seeds_gates... ");
   test_prepare_claude_seeds_gates();
   printf("OK\n");

   printf("test_prepare_claude_preserves_existing_settings... ");
   test_prepare_claude_preserves_existing_settings();
   printf("OK\n");

   printf("test_prepare_claude_skips_unparseable_config... ");
   test_prepare_claude_skips_unparseable_config();
   printf("OK\n");

   printf("test_prepare_claude_nonautonomous_skips_bypass_seed... ");
   test_prepare_claude_nonautonomous_skips_bypass_seed();
   printf("OK\n");

   printf("test_extract_claude_basic... ");
   test_extract_claude_basic();
   printf("OK\n");

   printf("test_extract_claude_excludes_prior_turn... ");
   test_extract_claude_excludes_prior_turn();
   printf("OK\n");

   printf("test_extract_claude_multiline... ");
   test_extract_claude_multiline();
   printf("OK\n");

   printf("test_extract_claude_json_hard_wrap_inside_string... ");
   test_extract_claude_json_hard_wrap_inside_string();
   printf("OK\n");

   printf("test_extract_claude_skips_effort_status... ");
   test_extract_claude_skips_effort_status();
   printf("OK\n");

   printf("test_extract_claude_effort_in_answer_kept... ");
   test_extract_claude_effort_in_answer_kept();
   printf("OK\n");

   printf("test_extract_codex_basic... ");
   test_extract_codex_basic();
   printf("OK\n");

   printf("test_extract_claude_multibullet... ");
   test_extract_claude_multibullet();
   printf("OK\n");

   printf("test_extract_baseline_substring_kept... ");
   test_extract_baseline_substring_kept();
   printf("OK\n");

   printf("test_extract_excludes_gerund_spinner... ");
   test_extract_excludes_gerund_spinner();
   printf("OK\n");

   printf("test_extract_excludes_middot_spinner_and_tip... ");
   test_extract_excludes_middot_spinner_and_tip();
   printf("OK\n");

   printf("test_extract_excludes_interrupt_footer... ");
   test_extract_excludes_interrupt_footer();
   printf("OK\n");

   printf("test_extract_welcome_banner_empty... ");
   test_extract_welcome_banner_empty();
   printf("OK\n");

   printf("test_extract_scrolled_answer_survives_fallback... ");
   test_extract_scrolled_answer_survives_fallback();
   printf("OK\n");

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

   printf("test_execution_name_isolates_override_less_delegates... ");
   test_execution_name_isolates_override_less_delegates();
   printf("OK\n");

   printf("test_execution_name_preserves_primary_reuse... ");
   test_execution_name_preserves_primary_reuse();
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

   printf("test_create_multiword_cli_cmd_single_arg... ");
   test_create_multiword_cli_cmd_single_arg();
   printf("OK\n");

   printf("test_recv_timeout_on_changing_pane... ");
   test_recv_timeout_on_changing_pane();
   printf("OK\n");

   printf("test_recv_ok_on_stable_pane... ");
   test_recv_ok_on_stable_pane();
   test_recv_drives_heartbeat();
   printf("OK\n");

   printf("test_capture_joins_wraps_and_includes_scrollback... ");
   test_capture_joins_wraps_and_includes_scrollback();
   printf("OK\n");

   printf("test_recv_does_not_return_static_prompt_as_answer... ");
   test_recv_does_not_return_static_prompt_as_answer();
   printf("OK\n");

   printf("test_recv_dead_session... ");
   test_recv_dead_session();
   printf("OK\n");

   printf("test_recv_busy_footer_not_finalized... ");
   test_recv_busy_footer_not_finalized();
   printf("OK\n");

   printf("test_recv_cancel_claude_escape... ");
   test_recv_cancel_claude_escape();
   printf("OK\n");

   printf("test_recv_cancel_codex_generating_ctrlc... ");
   test_recv_cancel_codex_generating_ctrlc();
   printf("OK\n");

   printf("test_recv_cancel_codex_idle_skips_ctrlc... ");
   test_recv_cancel_codex_idle_skips_ctrlc();
   printf("OK\n");

   printf("test_recv_provider_error_returns_minus4... ");
   test_recv_provider_error_returns_minus4();
   printf("OK\n");

   printf("test_recv_provider_error_disabled_times_out... ");
   test_recv_provider_error_disabled_times_out();
   printf("OK\n");

   printf("test_recv_banner_retry_not_provider_error... ");
   test_recv_banner_retry_not_provider_error();
   printf("OK\n");

   printf("All cli_session tests passed.\n");
   return 0;
}
