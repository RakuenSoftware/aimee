/* test_roundtable_pipeline_chunk.c: chunk planning, origin verification, and
 * synthesis assembly (#28/#32/#34/#37/#39), plus panel diversity + context
 * budget resolution (section 7/#36). Pure logic. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "roundtable_pipeline_chunk.h"
#include "roundtable_pipeline_eval.h"

#include "support/module_bus_stub.h"

#include <aimee/roundtable/module_api.h>

/* A plan the module would return for a four-line origin split at 10 bytes. */
#define PLAN_TWO_SPANS                                                                             \
   "{\"plan\":{\"chunks\":[{\"index\":0,\"offset\":0,\"len\":10},"                                 \
   "{\"index\":1,\"offset\":10,\"len\":10}],\"count\":2,\"over_budget\":false,"                    \
   "\"truncated\":false},\"assembly\":{\"selected\":[0,1],\"omitted\":[],"                         \
   "\"over_budget\":false}}"

static const char *FOUR_LINES = "aaaa\nbbbb\ncccc\ndddd\n"; /* 20 bytes */

static void test_chunk_plan_seam(void)
{
   rtp_chunk_plan_t p;

   /* The module's spans are adopted, and the digests are filled HERE: the hash
    * never crossed the boundary, so a chunk hash must still verify against the
    * origin it was cut from. */
   module_bus_stub_reply(PLAN_TWO_SPANS);
   assert(rtp_chunk_plan(FOUR_LINES, 10, &p) == 0);
   assert(module_bus_stub_last_event() == AIMEE_ROUNDTABLE_EVENT_CHUNK_PLAN);
   assert(module_bus_stub_last_stage() == AIMEE_ROUNDTABLE_STAGE_CHUNK_PLAN);
   assert(p.count == 2);
   assert(p.chunks[0].offset == 0 && p.chunks[0].len == 10);
   assert(p.chunks[1].offset == 10 && p.chunks[1].len == 10);
   assert(p.origin_len == 20);
   assert(p.origin_hash[0] && p.chunks[0].hash[0] && p.chunks[1].hash[0]);
   assert(rtp_chunk_verify(FOUR_LINES, &p) == 1);

   /* Plan and assembly arrive together, so the pipeline carries the artifact
    * once rather than twice. */
   rtp_assembly_t a;
   module_bus_stub_reply(PLAN_TWO_SPANS);
   assert(rtp_chunk_plan_with_assembly(FOUR_LINES, 10, 100, &p, &a) == 0);
   assert(a.selected_count == 2 && a.omitted_count == 0);
   assert(a.used_bytes == 20);

   printf("  chunk plan seam: ok\n");
}

/* A span outside the artifact would hash and slice out of bounds, so it is
 * refused rather than clamped. Every refusal lands on the whole-artifact
 * fallback: a single chunk covering everything, which loses no content. Zero
 * chunks would make an unreachable module look like an empty artifact. */
static void test_chunk_plan_fallback(void)
{
   static const char *bad[] = {
       "{\"plan\":{\"chunks\":[{\"index\":0,\"offset\":-1,\"len\":5}]}}",
       "{\"plan\":{\"chunks\":[{\"index\":0,\"offset\":0,\"len\":999}]}}",
       "{\"plan\":{\"chunks\":[{\"index\":0,\"offset\":25,\"len\":1}]}}",
       "{\"plan\":{\"chunks\":[]}}",
       "{\"plan\":{}}",
       "{}",
       "not json",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      rtp_chunk_plan_t p;
      module_bus_stub_reply(bad[i]);
      assert(rtp_chunk_plan(FOUR_LINES, 10, &p) == 0);
      assert(p.count == 1);
      assert(p.chunks[0].offset == 0 && p.chunks[0].len == 20);
      assert(rtp_chunk_verify(FOUR_LINES, &p) == 1);
   }

   rtp_chunk_plan_t p;
   module_bus_stub_absent();
   assert(rtp_chunk_plan(FOUR_LINES, 10, &p) == 0);
   assert(p.count == 1 && p.chunks[0].len == 20);

   module_bus_stub_fail(AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(rtp_chunk_plan(FOUR_LINES, 10, &p) == 0);
   assert(p.count == 1 && p.chunks[0].len == 20);
   assert(rtp_chunk_verify(FOUR_LINES, &p) == 1);

   printf("  chunk plan fallback: ok\n");
}

/* rtp_chunk_needed never crossed the boundary: it is a length comparison the
 * caller makes before deciding to ask anything. */
static void test_chunk_needed(void)
{
   assert(rtp_chunk_needed("short", 100) == 0);
   assert(rtp_chunk_needed(FOUR_LINES, 10) == 1);
   assert(rtp_chunk_needed(FOUR_LINES, 0) == 0);
   assert(rtp_chunk_needed(NULL, 10) == 0);
   printf("  chunk needed: ok\n");
}

/* Verification must REJECT a plan whose spans no longer hash to the origin --
 * that is the staleness guard the whole derive-from-origin design rests on. */
static void test_chunk_verify_rejects_stale(void)
{
   rtp_chunk_plan_t p;
   module_bus_stub_reply(PLAN_TWO_SPANS);
   assert(rtp_chunk_plan(FOUR_LINES, 10, &p) == 0);
   assert(rtp_chunk_verify("aaaa\nbbbb\ncccc\nXXXX\n", &p) == 0);
   printf("  chunk verify rejects stale: ok\n");
}

static void test_assembly(void)
{
   char big[600];
   int n = 0;
   for (int i = 0; i < 20; i++)
      n += snprintf(big + n, sizeof(big) - n, "line %02d\n", i);
   /* rtp_assembly_build still selects from a plan the caller already holds, so
    * it is tested against a plan the module returned rather than one this file
    * computes: the splitting itself is pinned module-side now. */
   module_bus_stub_reply("{\"plan\":{\"chunks\":["
                         "{\"index\":0,\"offset\":0,\"len\":40},"
                         "{\"index\":1,\"offset\":40,\"len\":40},"
                         "{\"index\":2,\"offset\":80,\"len\":40},"
                         "{\"index\":3,\"offset\":120,\"len\":40}],"
                         "\"count\":4},\"assembly\":{}}");
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
   test_chunk_plan_seam();
   test_chunk_plan_fallback();
   test_chunk_needed();
   test_chunk_verify_rejects_stale();
   test_assembly();
   test_panel();
   printf("test_roundtable_pipeline_chunk: all passed\n");
   return 0;
}
