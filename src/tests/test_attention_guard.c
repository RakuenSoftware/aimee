/* test_attention_guard.c: unit tests for the P3 attention-guard pure helpers
 * (scoring with recency decay, op classification, kind weights). The stateful
 * hook handler (handle_attention_guard) is exercised by a scripted functional
 * smoke, not here; stubs satisfy the linker for its unused dependencies. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli_attention_guard.h"

/* Stubs for handle_attention_guard's deps (it is not called here). */
char *read_stdin(void)
{
   return NULL;
}
const char *aimee_home(void)
{
   return "/tmp";
}
int platform_mkdir_p(const char *path, int mode)
{
   (void)path;
   (void)mode;
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

int main(void)
{
   printf("attention_guard: ");
   test_classify();
   test_weight();
   test_score();
   printf("all tests passed\n");
   return 0;
}
