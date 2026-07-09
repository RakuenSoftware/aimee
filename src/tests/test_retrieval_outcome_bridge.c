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

/* Accumulate the ids written under each verdict, so per-doc partitioning can be
 * verified across the (up to two) batched calls per surface. */
static int64_t g_acc_ids[16];
static int g_acc_n;
static int64_t g_neg_ids[16];
static int g_neg_n;

int kb_client_record_retrieval_outcome(const char *surface, const char *event_id,
                                       const int64_t *ids, int n, const char *verdict)
{
   snprintf(last_surface, sizeof(last_surface), "%s", surface ? surface : "");
   snprintf(last_event, sizeof(last_event), "%s", event_id ? event_id : "");
   snprintf(last_verdict, sizeof(last_verdict), "%s", verdict ? verdict : "");
   last_n = n;
   call_count++;
   int64_t *dst = (verdict && strcmp(verdict, "accepted") == 0) ? g_acc_ids : g_neg_ids;
   int *dn = (verdict && strcmp(verdict, "accepted") == 0) ? &g_acc_n : &g_neg_n;
   for (int i = 0; ids && i < n && *dn < 16; i++)
      dst[(*dn)++] = ids[i];
   return 0;
}

static int has_id(const int64_t *a, int n, int64_t v)
{
   for (int i = 0; i < n; i++)
      if (a[i] == v)
         return 1;
   return 0;
}

static void reset_capture(void)
{
   last_surface[0] = last_event[0] = last_verdict[0] = '\0';
   last_n = 0;
   call_count = 0;
   g_acc_n = 0;
   g_neg_n = 0;
   retrieval_outcome_bridge_reset();
}

/* Flat (no snippet / no prior answer) — the B2 behaviour, preserved. */
static void test_continuation_accepted(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[2] = {10, 11};
   retrieval_outcome_bridge_note("memory", "ev-1", ids, NULL, 2);
   retrieval_outcome_bridge_on_autolabel(NULL, 1 /*continuation*/, 0);
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
   retrieval_outcome_bridge_note("ranker", "ev-2", ids, NULL, 1);
   retrieval_outcome_bridge_on_autolabel(NULL, 0, 1 /*repair*/);
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
   retrieval_outcome_bridge_note("memory", "ev-m", m, NULL, 1);
   retrieval_outcome_bridge_note("ranker", "ev-r", r, NULL, 1);
   retrieval_outcome_bridge_on_autolabel(NULL, 1, 0);
   assert(call_count == 2); /* one per surface */
   printf("  both_surfaces: ok\n");
}

static void test_gate_off(void)
{
   reset_capture();
   g_flag = 0; /* flag off */
   int64_t ids[1] = {5};
   retrieval_outcome_bridge_note("memory", "ev-x", ids, NULL, 1);
   retrieval_outcome_bridge_on_autolabel(NULL, 1, 0);
   assert(call_count == 0); /* no write when disabled */
   printf("  gate_off: ok\n");
}

static void test_consume_once(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[1] = {7};
   retrieval_outcome_bridge_note("memory", "ev-c", ids, NULL, 1);
   /* Turn with no clear label: drops the note. */
   retrieval_outcome_bridge_on_autolabel(NULL, 0, 0);
   assert(call_count == 0);
   /* A later labelled turn must NOT resurrect the dropped note. */
   retrieval_outcome_bridge_on_autolabel(NULL, 1, 0);
   assert(call_count == 0);
   printf("  consume_once: ok\n");
}

static void test_no_pending(void)
{
   reset_capture();
   g_flag = 1;
   retrieval_outcome_bridge_on_autolabel(NULL, 1, 0);
   assert(call_count == 0);
   printf("  no_pending: ok\n");
}

/* The pure overlap test: an answer that reuses a snippet's content words. */
static void test_overlap_used(void)
{
   assert(retrieval_outcome_overlap_used(
              "The three-database split isolates the vector store from the graph.",
              "three-database split isolates vector store") == 1);
   assert(retrieval_outcome_overlap_used("Completely unrelated pizza recipe instructions here.",
                                         "kubernetes ingress controller sidecar topology") == 0);
   assert(retrieval_outcome_overlap_used(NULL, "x") == 0);
   assert(retrieval_outcome_overlap_used("x", NULL) == 0);
   printf("  overlap_used: ok\n");
}

/* Per-doc: a good (continuation) answer that used doc A's content but not doc B's
 * -> A accepted, B corrected. This is the within-query contrast a ranker needs. */
static void test_perdoc_continuation_contrast(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[2] = {201, 202};
   const char *snips[2] = {"retrieval outcome bridge attribution overlap",
                           "unrelated quarterly budget spreadsheet figures"};
   retrieval_outcome_bridge_note("ranker", "ev-pd", ids, snips, 2);
   retrieval_outcome_bridge_on_autolabel(
       "I used the retrieval outcome bridge to compute overlap attribution.", 1 /*continuation*/,
       0);
   /* doc 201 (used) accepted; doc 202 (unused) corrected. */
   assert(has_id(g_acc_ids, g_acc_n, 201));
   assert(!has_id(g_acc_ids, g_acc_n, 202));
   assert(has_id(g_neg_ids, g_neg_n, 202));
   assert(!has_id(g_neg_ids, g_neg_n, 201));
   printf("  perdoc_continuation_contrast: ok\n");
}

/* Per-doc under repair: only the doc the (bad) answer actually used is blamed;
 * the unused doc carries no signal and is dropped. */
static void test_perdoc_repair_blames_used(void)
{
   reset_capture();
   g_flag = 1;
   int64_t ids[2] = {301, 302};
   const char *snips[2] = {"widget frobnicator calibration procedure",
                           "entirely different photosynthesis chlorophyll notes"};
   retrieval_outcome_bridge_note("memory", "ev-r2", ids, snips, 2);
   retrieval_outcome_bridge_on_autolabel("Per the widget frobnicator calibration procedure, do X.",
                                         0, 1 /*repair*/);
   assert(has_id(g_neg_ids, g_neg_n, 301));  /* used -> blamed */
   assert(!has_id(g_neg_ids, g_neg_n, 302)); /* unused -> dropped */
   assert(g_acc_n == 0);                     /* nothing accepted on a repair */
   printf("  perdoc_repair_blames_used: ok\n");
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
   test_overlap_used();
   test_perdoc_continuation_contrast();
   test_perdoc_repair_blames_used();
   printf("test_retrieval_outcome_bridge: all passed\n");
   return 0;
}
