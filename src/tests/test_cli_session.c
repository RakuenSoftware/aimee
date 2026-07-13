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
#include <sys/stat.h>
#include "cJSON.h"
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

/* Path the fake tmux appends every `new-session` argv element to (one per
 * line, bracketed), so the create test can assert the pane command reached
 * tmux as a single intact argument rather than word-split fragments. */
static char g_createlog[300];

/* Path the fake tmux appends every `send-keys` invocation to, so a test can
 * assert which interrupt key (if any) the cancel path sent. */
static char g_sendlog[320];

static void install_fake_tmux(void)
{
   snprintf(g_fake_dir, sizeof(g_fake_dir), "/tmp/aimee_faketmux_%d", (int)getpid());
   mkdir(g_fake_dir, 0700);
   char counter[300];
   snprintf(counter, sizeof(counter), "%s/counter", g_fake_dir);
   snprintf(g_sendlog, sizeof(g_sendlog), "%s/sendlog", g_fake_dir);
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
    * "esc to interrupt" (a long-running tool — recv must NOT finalize it); anything
    * else returns a static pane. send-keys is logged so the cancel/error tests can
    * assert the interrupt key. */
   fprintf(f,
           "#!/bin/sh\n"
           "case \"$1\" in\n"
           "  has-session) [ \"$FAKE_TMUX_MODE\" = dead ] && exit 1; exit 0 ;;\n"
           "  capture-pane)\n"
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
           "    else echo 'STATIC OUTPUT'; fi; exit 0 ;;\n"
           "  send-keys) shift; echo \"$*\" >> '%s'; exit 0 ;;\n"
           "  new-session) shift; for a in \"$@\"; do echo \"ARG:[$a]\" >> '%s'; done; exit 0 ;;\n"
           "  *) exit 0 ;;\n"
           "esac\n",
           counter, counter, counter, counter, counter, counter, counter, counter, counter,
           g_sendlog, g_createlog);
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

/* cli_session_create must hand tmux the pane command (and workdir) as ONE
 * argument each. cli_cmd is multi-word in production (the AIMEE_SESSION_ID
 * stamp, --model, --dangerously-skip-permissions): unquoted interpolation gets
 * word-split by the outer shell, tmux re-joins the fragments, and the pane's
 * `sh -c` executes only the leading env assignment — exiting 0 instantly and
 * surfacing as "failed to send prompt to tmux session". */
static void test_create_multiword_cli_cmd_single_arg(void)
{
   unlink(g_createlog);
   cli_session_t s;
   int rc = cli_session_create(&s, "aimee-quoting-test",
                               "AIMEE_SESSION_ID=web-1 claude --dangerously-skip-permissions",
                               "/tmp/aimee quoting wd", 0);
   assert(rc == 0);
   assert(createlog_has("ARG:[AIMEE_SESSION_ID=web-1 claude --dangerously-skip-permissions]"));
   assert(createlog_has("ARG:[/tmp/aimee quoting wd]"));
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
   assert(elapsed < 5000);
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
   assert(elapsed < 5000);        /* bounded by the 800ms grace, not the 30s timeout */
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
   char home[] = "/tmp/aimee_clitest_XXXXXX";
   assert(mkdtemp(home) != NULL);
   setenv("HOME", home, 1); /* cli_claude_home() resolves HOME first */
   setenv("AIMEE_HOME", home, 1);
   const char *wt = "/tmp/aimee_clitest_wt/session-abc";

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
   char home[] = "/tmp/aimee_clitest_XXXXXX";
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

   cli_session_prepare_claude("/tmp/aimee_clitest_wt2", 1);

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
   char home[] = "/tmp/aimee_clitest_XXXXXX";
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

   cli_session_prepare_claude("/tmp/aimee_clitest_wt3", 1);

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
   char home[] = "/tmp/aimee_clitest_XXXXXX";
   assert(mkdtemp(home) != NULL);
   setenv("HOME", home, 1);
   setenv("AIMEE_HOME", home, 1);
   const char *wt = "/tmp/aimee_clitest_wt4";

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

int main(void)
{
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
