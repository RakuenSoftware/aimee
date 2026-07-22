/* Unit tests for the replay-verification pass + rubric (Part A WP-A3/A4).
 * Pure: an injected fake replay backend drives the logic, so the link needs no DB. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roundtable_verify.h"

/* fake backend: callers reproduce 3 hits unless the symbol is "absent" (0). */
static int g_fpc = 1; /* project_count: set 0 to simulate an unavailable index */
static int fpc(void)
{
   return g_fpc;
}
static int fcallers(const char *p, const char *s, caller_hit_t *o, int m)
{
   (void)p;
   if (strcmp(s, "absent") == 0)
      return 0;
   int n = 3 < m ? 3 : m;
   for (int i = 0; i < n; i++)
   {
      snprintf(o[i].file_path, sizeof(o[i].file_path), "src/c_%d.c", i);
      o[i].line = 1 + i;
   }
   return n;
}
static replay_backend_t fake(void)
{
   replay_backend_t b = {0};
   b.find_callers = fcallers;
   b.project_count = fpc;
   return b;
}

static void test_rubric(void)
{
   char sev[16];
   assert(roundtable_grade_item(REPLAY_CONTRADICTED, 1, "blocking", sev, sizeof(sev)) ==
          VERIFY_REJECT);
   assert(roundtable_grade_item(REPLAY_VACUOUS, 1, "blocking", sev, sizeof(sev)) == VERIFY_REJECT);

   assert(roundtable_grade_item(REPLAY_INDEX_UNAVAILABLE, 1, "blocking", sev, sizeof(sev)) ==
          VERIFY_DEGRADE);
   assert(strcmp(sev, "blocking") == 0); /* degrade does not penalize */

   assert(roundtable_grade_item(REPLAY_NO_EVIDENCE, 0, "blocking", sev, sizeof(sev)) == VERIFY_CAP);
   assert(strcmp(sev, "suggestion") == 0); /* interpretive can never be blocking */

   assert(roundtable_grade_item(REPLAY_MATCH, 1, "blocking", sev, sizeof(sev)) == VERIFY_KEEP);
   assert(strcmp(sev, "blocking") == 0); /* reproduced factual trigger stands */

   /* reproduced but not asserted factual -> capped, never escalated */
   assert(roundtable_grade_item(REPLAY_MATCH, 0, "blocking", sev, sizeof(sev)) == VERIFY_CAP);
   assert(strcmp(sev, "suggestion") == 0);

   assert(roundtable_grade_item(REPLAY_CORRECTED, 1, "nit", sev, sizeof(sev)) == VERIFY_KEEP);
   assert(strcmp(sev, "nit") == 0);
}

static void set_item(roundtable_review_item_t *it, const char *sev, const char *sum, ev_kind_t k,
                     const char *target, int factual)
{
   memset(it, 0, sizeof(*it));
   snprintf(it->severity, sizeof(it->severity), "%s", sev);
   snprintf(it->summary, sizeof(it->summary), "%s", sum);
   snprintf(it->category, sizeof(it->category), "bug");
   it->evidence.kind = k;
   it->evidence.factual = factual;
   if (target)
      snprintf(it->evidence.target, sizeof(it->evidence.target), "%s", target);
}

static void test_verify_items(void)
{
   roundtable_result_t *r = calloc(1, sizeof(*r));
   assert(r);
   replay_backend_t be = fake();

   set_item(&r->items[0], "blocking", "real refs claim", EV_REFS, "present", 1);
   set_item(&r->items[1], "blocking", "bogus claim", EV_REFS, "absent", 1);
   set_item(&r->items[2], "blocking", "opinion", EV_NONE, NULL, 0);
   r->item_count = 3;

   roundtable_verify_items_with(r, &be, 0);

   assert(r->item_count == 2); /* the contradicted item is removed */
   assert(r->rejected_count == 1);
   assert(strcmp(r->rejected[0].summary, "bogus claim") == 0);
   assert(strcmp(r->rejected_reason[0], "contradicted") == 0);
   assert(r->verified_count == 1); /* the reproduced factual item */
   assert(r->capped_count == 1);   /* the interpretive item */

   /* kept order: verified item first (blocking), interpretive second (capped) */
   int saw_blocking = 0, saw_capped = 0;
   for (int i = 0; i < r->item_count; i++)
   {
      if (strcmp(r->items[i].summary, "real refs claim") == 0)
         saw_blocking = (strcmp(r->items[i].severity, "blocking") == 0);
      if (strcmp(r->items[i].summary, "opinion") == 0)
         saw_capped = (strcmp(r->items[i].severity, "suggestion") == 0);
   }
   assert(saw_blocking && saw_capped);
   free(r);
}

static void test_dedup_by_idkey(void)
{
   roundtable_result_t *r = calloc(1, sizeof(*r));
   assert(r);
   replay_backend_t be = fake();

   /* two items reproduce the same caller set -> same idkey -> one finding,
    * and the surviving item keeps BOTH panelists' attribution. */
   set_item(&r->items[0], "blocking", "from panelist A", EV_REFS, "present", 1);
   snprintf(r->items[0].sources, sizeof(r->items[0].sources), "minimax");
   set_item(&r->items[1], "blocking", "from panelist B", EV_REFS, "present", 1);
   snprintf(r->items[1].sources, sizeof(r->items[1].sources), "mistral");
   r->item_count = 2;

   roundtable_verify_items_with(r, &be, 0);
   assert(r->item_count == 1); /* deduped */
   assert(r->verified_count == 1);
   assert(r->rejected_count == 0);
   assert(strstr(r->items[0].sources, "minimax")); /* sources merged, not dropped */
   assert(strstr(r->items[0].sources, "mistral"));
   free(r);
}

static void test_degrade_when_index_unavailable(void)
{
   roundtable_result_t *r = calloc(1, sizeof(*r));
   assert(r);
   replay_backend_t be = fake();
   g_fpc = 0; /* index unavailable: every item degrades, none rejected */
   set_item(&r->items[0], "blocking", "claim a", EV_REFS, "present", 1);
   set_item(&r->items[1], "blocking", "claim b", EV_REFS, "absent", 1);
   r->item_count = 2;

   roundtable_verify_items_with(r, &be, 0);
   g_fpc = 1;                      /* restore for other tests */
   assert(r->item_count == 2);     /* both kept */
   assert(r->rejected_count == 0); /* nothing rejected when we cannot verify */
   assert(r->degraded_count == 2);
   assert(strcmp(r->items[0].severity, "blocking") == 0); /* degrade does not penalize */
   free(r);
}

static void test_render_rejected(void)
{
   roundtable_result_t *r = calloc(1, sizeof(*r));
   assert(r);
   replay_backend_t be = fake();
   set_item(&r->items[0], "blocking", "bogus", EV_REFS, "absent", 1);
   set_item(&r->items[1], "blocking", "good", EV_REFS, "present", 1);
   r->item_count = 2;
   roundtable_verify_items_with(r, &be, 0);

   char *md = roundtable_render_rejected(r);
   assert(md);
   assert(strstr(md, "Replay verification"));
   assert(strstr(md, "verified=1"));
   assert(strstr(md, "rejected=1"));
   assert(strstr(md, "Rejected at verification"));
   assert(strstr(md, "bogus"));
   free(md);

   /* append-in-place into an artifact */
   char *art = strdup("## Findings\n- something\n");
   roundtable_artifact_append_rejected(&art, r);
   assert(strstr(art, "## Findings"));
   assert(strstr(art, "Replay verification"));
   free(art);
   free(r);
}

static void test_render_nothing(void)
{
   roundtable_result_t *r = calloc(1, sizeof(*r));
   assert(r);
   /* no rejected / capped / degraded -> NULL appendix, artifact untouched */
   assert(roundtable_render_rejected(r) == NULL);
   char *art = strdup("x");
   roundtable_artifact_append_rejected(&art, r);
   assert(strcmp(art, "x") == 0);
   free(art);
   free(r);
}

/* The evidence gate: with require_evidence=1 a no-structured-evidence finding is
 * REJECTED (not kept-and-capped), so an unfalsifiable opinion never reaches synthesis. */
static void test_evidence_gate(void)
{
   replay_backend_t be = fake();

   /* gate OFF: the EV_NONE opinion is capped and kept (legacy behavior). */
   roundtable_result_t *r = calloc(1, sizeof(*r));
   assert(r);
   set_item(&r->items[0], "blocking", "real refs claim", EV_REFS, "present", 1);
   set_item(&r->items[1], "suggestion", "opinion", EV_NONE, NULL, 0);
   r->item_count = 2;
   roundtable_verify_items_with(r, &be, 0);
   assert(r->item_count == 2 && r->rejected_count == 0 && r->capped_count == 1);
   free(r);

   /* gate ON: the same EV_NONE opinion is rejected; the evidence-backed item survives. */
   r = calloc(1, sizeof(*r));
   assert(r);
   set_item(&r->items[0], "blocking", "real refs claim", EV_REFS, "present", 1);
   set_item(&r->items[1], "suggestion", "opinion", EV_NONE, NULL, 0);
   r->item_count = 2;
   roundtable_verify_items_with(r, &be, 1);
   assert(r->item_count == 1);     /* only the evidence-backed finding remains */
   assert(r->rejected_count == 1); /* the opinion was rejected by the gate */
   assert(r->capped_count == 0);
   assert(strcmp(r->items[0].summary, "real refs claim") == 0);
   assert(strcmp(r->rejected[0].summary, "opinion") == 0);
   free(r);

   /* gate ON must NOT reject an evidence-BEARING item merely because the index is
    * unavailable (that is DEGRADE, not NO_EVIDENCE) — the gate only drops kind=none. */
   r = calloc(1, sizeof(*r));
   assert(r);
   g_fpc = 0; /* index unavailable -> replay yields INDEX_UNAVAILABLE -> DEGRADE */
   set_item(&r->items[0], "blocking", "refs claim, index down", EV_REFS, "present", 1);
   r->item_count = 1;
   roundtable_verify_items_with(r, &be, 1);
   g_fpc = 1; /* restore */
   assert(r->item_count == 1 && r->rejected_count == 0 && r->degraded_count == 1);
   assert(strcmp(r->items[0].severity, "blocking") == 0); /* degrade does not penalize */
   free(r);
}

int main(void)
{
   test_rubric();
   test_verify_items();
   test_dedup_by_idkey();
   test_degrade_when_index_unavailable();
   test_render_rejected();
   test_render_nothing();
   test_evidence_gate();
   printf("roundtable_verify: all tests passed\n");
   return 0;
}
