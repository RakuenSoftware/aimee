/* test_retrieval_outcome_bridge.c — the dogfood-autolabel -> retrieval outcome
 * bridge. Stubs the KB client + config_load so the buffer / verdict / consume
 * logic is exercised in isolation (no DB, no network).
 *
 * Covers:
 *   1. continuation -> accepted, memory surface, prior rows.
 *   2. repair -> corrected, ranker surface.
 *   3. both surfaces attributed in one turn.
 *   4. gate: flag off -> no write.
 *   5. consume-once: an unlabelled turn drops the note (no later mis-attribution).
 *   6. no pending -> no write.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../headers/config.h"
#include "../server/retrieval_outcome_bridge.h"

/* ---- test-controlled config ---- */
static int g_flag = 1;
int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->learning_implicit_retrieval_outcome = g_flag;
   return 0;
}

/* ---- capture stub for the KB client record call ---- */
static char last_surface[16];
static char last_event[64];
static char last_verdict[32];
static int last_n;
static int call_count;

int kb_client_record_retrieval_outcome(const char *surface, const char *event_id,
                                       const int64_t *ids, int n, const char *verdict)
{
   (void)ids;
   snprintf(last_surface, sizeof(last_surface), "%s", surface ? surface : "");
   snprintf(last_event, sizeof(last_event), "%s", event_id ? event_id : "");
   snprintf(last_verdict, sizeof(last_verdict), "%s", verdict ? verdict : "");
   last_n = n;
   call_count++;
   return 0;
}

static void reset_capture(void)
{
   last_surface[0] = last_event[0] = last_verdict[0] = '\0';
   last_n = 0;
   call_count = 0;
   retrieval_outcome_bridge_reset();
}

static void test_continuation_accepted(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[2] = {10, 11};
   retrieval_outcome_bridge_note("memory", "ev-1", ids, 2);
   retrieval_outcome_bridge_on_autolabel(1 /*continuation*/, 0);
   assert(call_count == 1);
   assert(strcmp(last_surface, "memory") == 0);
   assert(strcmp(last_event, "ev-1") == 0);
   assert(strcmp(last_verdict, "accepted") == 0);
   assert(last_n == 2);
   printf("  continuation_accepted: ok\n");
}

static void test_repair_corrected_ranker(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[1] = {100};
   retrieval_outcome_bridge_note("ranker", "ev-2", ids, 1);
   retrieval_outcome_bridge_on_autolabel(0, 1 /*repair*/);
   assert(call_count == 1);
   assert(strcmp(last_surface, "ranker") == 0);
   assert(strcmp(last_verdict, "corrected") == 0);
   printf("  repair_corrected_ranker: ok\n");
}

static void test_both_surfaces(void)
{
   reset_capture();
   g_flag = 1;
   int64_t m[1] = {1}, r[1] = {2};
   retrieval_outcome_bridge_note("memory", "ev-m", m, 1);
   retrieval_outcome_bridge_note("ranker", "ev-r", r, 1);
   retrieval_outcome_bridge_on_autolabel(1, 0);
   assert(call_count == 2); /* one per surface */
   printf("  both_surfaces: ok\n");
}

static void test_gate_off(void)
{
   reset_capture();
   g_flag = 0; /* flag off */
   int64_t ids[1] = {5};
   retrieval_outcome_bridge_note("memory", "ev-x", ids, 1);
   retrieval_outcome_bridge_on_autolabel(1, 0);
   assert(call_count == 0); /* no write when disabled */
   printf("  gate_off: ok\n");
}

static void test_consume_once(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[1] = {7};
   retrieval_outcome_bridge_note("memory", "ev-c", ids, 1);
   /* Turn with no clear label: drops the note. */
   retrieval_outcome_bridge_on_autolabel(0, 0);
   assert(call_count == 0);
   /* A later labelled turn must NOT resurrect the dropped note. */
   retrieval_outcome_bridge_on_autolabel(1, 0);
   assert(call_count == 0);
   printf("  consume_once: ok\n");
}

static void test_no_pending(void)
{
   reset_capture();
   g_flag = 1;
   retrieval_outcome_bridge_on_autolabel(1, 0);
   assert(call_count == 0);
   printf("  no_pending: ok\n");
}

int main(void)
{
   printf("test_retrieval_outcome_bridge:\n");
   test_continuation_accepted();
   test_repair_corrected_ranker();
   test_both_surfaces();
   test_gate_off();
   test_consume_once();
   test_no_pending();
   printf("test_retrieval_outcome_bridge: all passed\n");
   return 0;
}
