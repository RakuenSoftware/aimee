/* test_attention_guard.c: unit tests for the P3 attention-guard pure helpers
 * (scoring with recency decay, op classification, kind weights) plus a
 * functional test of the hook handler's raw-scan enforcement (inert by default,
 * blocking only at a positive ingress_max_raw_scans cap). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "cli_attention_guard.h"

/* Stubs/fakes for handle_attention_guard's deps. read_stdin + aimee_home are
 * driven by the functional test below via these globals. */
static const char *g_stdin_json = NULL;
static char g_home[256] = "/tmp";

char *read_stdin(void)
{
   return g_stdin_json ? strdup(g_stdin_json) : NULL;
}
const char *aimee_home(void)
{
   return g_home;
}
int platform_mkdir_p(const char *path, int mode)
{
   /* Real recursive mkdir so the handler can persist its per-session raw-scan
    * log (the cap test depends on that count surviving across invocations). */
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", path);
   for (char *p = buf + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(buf, (mode_t)mode);
         *p = '/';
      }
   }
   mkdir(buf, (mode_t)mode);
   return 0;
}

static void test_classify(void)
{
   assert(attn_classify("Read", NULL) == ATTN_OP_READ);
   assert(attn_classify("Edit", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("Write", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("MultiEdit", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("NotebookEdit", NULL) == ATTN_OP_SOFT);
   assert(attn_classify("Bash", "rm -rf src/x.c") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "rm -fr build") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "truncate -s0 log") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "shred secret") == ATTN_OP_HARD);
   assert(attn_classify("Bash", ": > file") == ATTN_OP_HARD);
   assert(attn_classify("Bash", "grep -R symbol .") == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Bash", "rg --files") == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Bash", "find . -name '*.c'") == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Bash", "rm stale.txt") == ATTN_OP_SOFT);  /* non-recursive */
   assert(attn_classify("Bash", "echo hi > out") == ATTN_OP_SOFT); /* redirect overwrite */
   assert(attn_classify("Bash", "ls -la") == ATTN_OP_READ);
   assert(attn_classify("Grep", NULL) == ATTN_OP_RAW_SCAN);
   assert(attn_classify("Glob", NULL) == ATTN_OP_RAW_SCAN);
   assert(attn_classify(NULL, NULL) == ATTN_OP_READ);
   assert(attn_is_raw_scan("Bash", "grep -r TODO src") == 1);
   assert(attn_is_raw_scan("Bash", "grep TODO src/file.c") == 0);
   printf("classify OK\n");
}

static void test_weight(void)
{
   assert(attn_weight_for(ATTN_OP_READ) == 2);
   assert(attn_weight_for(ATTN_OP_SOFT) == 8);
   assert(attn_weight_for(ATTN_OP_HARD) == 8);
   printf("weight OK\n");
}

static void test_score(void)
{
   long now = 1000000;
   attn_record_t recs[] = {
       {"a.c", 8, now},           /* fresh edit */
       {"b.c", 2, now},           /* fresh read */
       {"a.c", 2, now - 3600},    /* a read 1h ago -> decays to 1.0 */
       {"old.c", 8, now - 36000}, /* 10h ago -> 8 * 2^-10 ~ 0.0078 */
   };
   int n = (int)(sizeof(recs) / sizeof(recs[0]));

   /* fresh edit (8) + 1h-old read (2*0.5=1) = 9.0 */
   double a = attn_score(recs, n, "a.c", now);
   assert(fabs(a - 9.0) < 0.001);
   /* fresh read = 2.0 == threshold (high attention) */
   assert(fabs(attn_score(recs, n, "b.c", now) - 2.0) < 0.001);
   assert(attn_score(recs, n, "b.c", now) >= ATTN_HIGH_THRESHOLD);
   /* a 10h-old single edit is well below threshold */
   assert(attn_score(recs, n, "old.c", now) < ATTN_HIGH_THRESHOLD);
   /* unknown path -> 0 */
   assert(attn_score(recs, n, "missing.c", now) == 0.0);
   /* NULL safety */
   assert(attn_score(NULL, 0, "x", now) == 0.0);
   printf("score OK\n");
}

/* Hook input for a recursive raw scan (the Bash `grep -r` form). */
#define RAW_SCAN_HOOK                                                                              \
   "{\"session_id\":\"agtest\",\"tool_name\":\"Bash\","                                            \
   "\"tool_input\":{\"command\":\"grep -r TODO src\"}}"

static void write_config(const char *body)
{
   char path[320];
   snprintf(path, sizeof(path), "%s/aimee.yaml", g_home);
   FILE *f = fopen(path, "wb");
   assert(f);
   if (body)
      fputs(body, f);
   fclose(f);
}

static void rm_path(const char *p)
{
   remove(p);
}

/* Functional test of the raw-scan enforcement: inert unless a positive
 * ingress_max_raw_scans cap is configured. */
static void test_guard_enforcement(void)
{
   /* Isolated, real temp home so config + the session log persist. */
   snprintf(g_home, sizeof(g_home), "/tmp/aimee_ag_test_%d", (int)getpid());
   mkdir(g_home, 0700);
   char logpath[400], cfgpath[400];
   snprintf(logpath, sizeof(logpath), "%s/.cache/attention/agtest.json", g_home);
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", g_home);
   g_stdin_json = RAW_SCAN_HOOK;

   /* (1) Inert default: no aimee.yaml at all -> raw scans allowed (exit 0). */
   rm_path(cfgpath);
   rm_path(logpath);
   assert(handle_attention_guard() == 0);
   assert(handle_attention_guard() == 0); /* still allowed, repeatedly */

   /* (2) Explicit 0 is also disabled. */
   write_config("ingress_max_raw_scans: 0\n");
   rm_path(logpath);
   assert(handle_attention_guard() == 0);

   /* (3) Positive cap of 2: first two scans allowed, the third is blocked. */
   write_config("ingress_max_raw_scans: 2\n");
   rm_path(logpath);
   assert(handle_attention_guard() == 0); /* used 0 -> allow, count 1 */
   assert(handle_attention_guard() == 0); /* used 1 -> allow, count 2 */
   assert(handle_attention_guard() == 2); /* used 2 >= cap -> block */

   /* (4) AIMEE_GUARD=0 bypasses even with a cap hit. */
   setenv("AIMEE_GUARD", "0", 1);
   assert(handle_attention_guard() == 0);
   unsetenv("AIMEE_GUARD");

   rm_path(logpath);
   rm_path(cfgpath);
   g_stdin_json = NULL;
   printf("enforcement OK\n");
}

int main(void)
{
   printf("attention_guard: ");
   test_classify();
   test_weight();
   test_score();
   test_guard_enforcement();
   printf("all tests passed\n");
   return 0;
}
