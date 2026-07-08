/* test_curator_pipeline_sched.c: kb_curator_pipeline_run_pass scheduling — the guard for
 * the starvation fix. Mock stages verify every enabled stage advances each pass
 * (no downstream starvation), disabled/lane-filtered stages skip, a per-pass budget
 * drains a stage's queue, and a stage error stops the pass. No DB. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "kb/kb_curator_pipeline.h"

static int q_a, q_b, n_a, n_b, n_err, n_off;

static int run_a(const kb_curator_extract_opts_t *o)
{
   (void)o;
   n_a++;
   if (q_a > 0)
   {
      q_a--;
      return 1;
   }
   return 0;
}
static int run_b(const kb_curator_extract_opts_t *o)
{
   (void)o;
   n_b++;
   if (q_b > 0)
   {
      q_b--;
      return 1;
   }
   return 0;
}
static int run_err(const kb_curator_extract_opts_t *o)
{
   (void)o;
   n_err++;
   return -1;
}
static int run_off(const kb_curator_extract_opts_t *o)
{
   (void)o;
   n_off++;
   return 0;
}
static int en_true(const config_t *c)
{
   (void)c;
   return 1;
}
static int en_false(const config_t *c)
{
   (void)c;
   return 0;
}
static void reset(void)
{
   q_a = q_b = n_a = n_b = n_err = n_off = 0;
}

int main(void)
{
   printf("test_curator_pipeline:\n");
   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));

   const kb_curator_stage_desc_t st[] = {
       {"a", "a", en_true, run_a, 1, KB_CURATOR_LANE_LLM},
       {"b", "b", en_true, run_b, 32, KB_CURATOR_LANE_INDEX},
       {"off", "off", en_false, run_off, 1, KB_CURATOR_LANE_INDEX},
   };

   /* 1: enabled stages advance; disabled skip; INDEX budget drains its queue. */
   reset();
   q_a = 1;
   q_b = 5;
   int r = kb_curator_pipeline_run_pass(st, 3, -1, NULL, &opts, NULL);
   assert(r == 1);
   assert(n_a == 1 && q_a == 0); /* LLM stage: one unit this pass */
   assert(n_b == 6 && q_b == 0); /* INDEX stage: budget 32 drained 5 (6th call idle) */
   assert(n_off == 0);           /* disabled skipped */
   printf("  PASS: enabled advance, disabled skip, INDEX budget drains\n");

   /* 2: idle pass returns 0. */
   reset();
   r = kb_curator_pipeline_run_pass(st, 3, -1, NULL, &opts, NULL);
   assert(r == 0);
   printf("  PASS: idle pass returns 0\n");

   /* 3: lane filter runs only the matching lane (per-lane worker). */
   reset();
   q_a = 1;
   q_b = 3;
   r = kb_curator_pipeline_run_pass(st, 3, KB_CURATOR_LANE_INDEX, NULL, &opts, NULL);
   assert(r == 1);
   assert(n_a == 0); /* LLM stage filtered out */
   assert(q_b == 0); /* INDEX stage drained */
   printf("  PASS: lane filter isolates a lane\n");

   /* 4: a stage error stops the pass; later stages skip; returns -1. */
   reset();
   q_a = 1;
   q_b = 5;
   const kb_curator_stage_desc_t ste[] = {
       {"a", "a", en_true, run_a, 1, KB_CURATOR_LANE_LLM},
       {"err", "err", en_true, run_err, 1, KB_CURATOR_LANE_LLM},
       {"b", "b", en_true, run_b, 32, KB_CURATOR_LANE_INDEX},
   };
   r = kb_curator_pipeline_run_pass(ste, 3, -1, NULL, &opts, NULL);
   assert(r == -1);
   assert(n_a == 1 && n_err == 1 && n_b == 0); /* stopped at the error */
   printf("  PASS: stage error stops the pass\n");

   printf("test_curator_pipeline: all tests passed\n");
   return 0;
}
