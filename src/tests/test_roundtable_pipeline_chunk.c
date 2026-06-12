/* test_roundtable_pipeline_chunk.c: chunk planning, origin verification, and
 * synthesis assembly (#28/#32/#34/#37/#39), plus panel diversity + context
 * budget resolution (section 7/#36). Pure logic. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "roundtable_pipeline_chunk.h"
#include "roundtable_pipeline_eval.h"

static void test_chunk_plan(void)
{
   /* small artifact, generous budget -> single chunk that fits. */
   const char *small = "line one\nline two\n";
   assert(rtp_chunk_needed(small, 4096) == 0);
   rtp_chunk_plan_t p;
   assert(rtp_chunk_plan(small, 4096, &p) == 0);
   assert(p.count == 1);
   assert(p.chunks[0].offset == 0 && p.chunks[0].len == (int)strlen(small));
   assert(p.origin_hash[0] && p.chunks[0].hash[0]);
   assert(rtp_chunk_verify(small, &p) == 1);

   /* a larger artifact split on line boundaries. */
   char big[2048];
   int n = 0;
   for (int i = 0; i < 40; i++)
      n += snprintf(big + n, sizeof(big) - n, "this is line %02d of the artifact\n", i);
   assert(rtp_chunk_needed(big, 100) == 1);
   assert(rtp_chunk_plan(big, 100, &p) == 0);
   assert(p.count > 1);
   assert(p.over_budget == 0); /* every line fits in 100 bytes */
   assert(p.truncated == 0);
   /* chunks tile the origin exactly and end on newline boundaries. */
   int covered = 0;
   for (int i = 0; i < p.count; i++)
   {
      assert(p.chunks[i].offset == covered);
      assert(big[p.chunks[i].offset + p.chunks[i].len - 1] == '\n');
      covered += p.chunks[i].len;
   }
   assert(covered == (int)strlen(big));
   assert(rtp_chunk_verify(big, &p) == 1);

   /* a mutated origin must fail verification (#42 freshness). */
   char mutated[2048];
   snprintf(mutated, sizeof(mutated), "%s", big);
   mutated[10] = (mutated[10] == 'x') ? 'y' : 'x';
   assert(rtp_chunk_verify(mutated, &p) == 0);

   /* an indivisible over-long line trips over_budget, never crashes. */
   const char *longline = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 40, no newline */
   assert(rtp_chunk_plan(longline, 10, &p) == 0);
   assert(p.over_budget == 1);
   printf("  chunk plan + verify: ok\n");
}

static void test_assembly(void)
{
   char big[600];
   int n = 0;
   for (int i = 0; i < 20; i++)
      n += snprintf(big + n, sizeof(big) - n, "line %02d\n", i);
   rtp_chunk_plan_t p;
   assert(rtp_chunk_plan(big, 40, &p) == 0);
   assert(p.count >= 3);

   /* a synthesis budget that fits every chunk -> nothing omitted. */
   rtp_assembly_t a;
   assert(rtp_assembly_build(&p, 100000, &a) == 0);
   assert(a.selected_count == p.count);
   assert(a.omitted_count == 0);
   assert(a.over_budget == 0);

   /* a tight budget -> some chunks omitted, recorded as a coverage gap (#39). */
   assert(rtp_assembly_build(&p, 50, &a) == 0);
   assert(a.omitted_count > 0);
   assert(a.selected_count + a.omitted_count == p.count);
   printf("  synthesis assembly: ok\n");
}

static void test_panel(void)
{
   /* two distinct providers, both with known budgets. */
   rtp_participant_t parts[3] = {
       {"anthropic", 200000},
       {"openai", 128000},
       {"anthropic", 200000},
   };
   rtp_panel_t panel;
   assert(rtp_panel_summarize(parts, 3, 8000, &panel) == 0);
   assert(panel.resolved == 3);
   assert(panel.distinct_providers == 2);
   assert(panel.min_context_tokens == 128000); /* smallest resolved budget */
   assert(panel.used_fallback == 0);
   assert(rtp_panel_diverse(&panel) == 1);

   /* single provider -> not diverse (section 7 blind-spot guard). */
   rtp_participant_t mono[2] = {{"anthropic", 200000}, {"anthropic", 200000}};
   assert(rtp_panel_summarize(mono, 2, 8000, &panel) == 0);
   assert(panel.distinct_providers == 1);
   assert(rtp_panel_diverse(&panel) == 0);

   /* unknown budget + fallback -> fallback used as the min (#36). */
   rtp_participant_t unk[2] = {{"anthropic", 0}, {"openai", 128000}};
   assert(rtp_panel_summarize(unk, 2, 8000, &panel) == 0);
   assert(panel.used_fallback == 1);
   assert(panel.min_context_tokens == 8000);

   /* unknown budget + NO fallback -> hard validation error (#36). */
   assert(rtp_panel_summarize(unk, 2, 0, &panel) == -1);

   /* an unresolved participant name is counted. */
   rtp_participant_t miss[2] = {{NULL, 0}, {"openai", 128000}};
   assert(rtp_panel_summarize(miss, 2, 8000, &panel) == 0);
   assert(panel.unknown_unresolved == 1);
   assert(panel.resolved == 1);
   printf("  panel resolution: ok\n");
}

int main(void)
{
   test_chunk_plan();
   test_assembly();
   test_panel();
   printf("test_roundtable_pipeline_chunk: all passed\n");
   return 0;
}
