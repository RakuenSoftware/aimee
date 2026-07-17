#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h" /* MAX_PATH_LEN (pulled transitively by roundtable_chair.h) */
#include "roundtable_chair.h"

static void set_item(roundtable_review_item_t *it, const char *sev, const char *sum)
{
   memset(it, 0, sizeof(*it));
   snprintf(it->severity, sizeof(it->severity), "%s", sev);
   snprintf(it->summary, sizeof(it->summary), "%s", sum);
   snprintf(it->category, sizeof(it->category), "correctness");
}

static int has_item(const roundtable_result_t *r, const char *sum)
{
   for (int i = 0; i < r->item_count; i++)
      if (strcmp(r->items[i].summary, sum) == 0)
         return 1;
   return 0;
}
static const char *sev_of(const roundtable_result_t *r, const char *sum)
{
   for (int i = 0; i < r->item_count; i++)
      if (strcmp(r->items[i].summary, sum) == 0)
         return r->items[i].severity;
   return NULL;
}

int main(void)
{
   printf("roundtable_chair: ");

   /* --- prompt builder: lists the surviving items, empty when none --- */
   {
      roundtable_result_t *r = calloc(1, sizeof(*r));
      assert(roundtable_chair_build_prompt(r) == NULL); /* no items -> no prompt */
      set_item(&r->items[0], "blocking", "buffer overrun in foo");
      r->item_count = 1;
      char *p = roundtable_chair_build_prompt(r);
      assert(p && strstr(p, "buffer overrun in foo") && strstr(p, "DEMOTE") && strstr(p, "DROP"));
      free(p);
      free(r);
   }

   /* --- apply: drop moves to rejected (transparent), demote lowers, escalate ignored,
    *     unmatched ignored, keep untouched --- */
   {
      roundtable_result_t *r = calloc(1, sizeof(*r));
      set_item(&r->items[0], "blocking", "over-flagged race");     /* chair: drop */
      set_item(&r->items[1], "blocking", "real but not blocking"); /* chair: demote */
      set_item(&r->items[2], "nit", "typo");                       /* chair: escalate (ignored) */
      set_item(&r->items[3], "suggestion", "keep me");             /* no verdict -> keep */
      r->item_count = 4;

      const char *cj =
          "```json\n{\"verdicts\":["
          "{\"summary\":\"over-flagged race\",\"verdict\":\"drop\",\"rationale\":\"guarded by the "
          "outer lock\"},"
          "{\"summary\":\"real but not blocking\",\"verdict\":\"demote\",\"new_severity\":"
          "\"suggestion\",\"rationale\":\"cosmetic\"},"
          "{\"summary\":\"typo\",\"verdict\":\"demote\",\"new_severity\":\"blocking\","
          "\"rationale\":"
          "\"should escalate\"},"
          "{\"summary\":\"a finding that does not exist\",\"verdict\":\"drop\",\"rationale\":\"x\"}"
          "]}\n```";
      int changed = 0;
      char *adj = roundtable_chair_apply(r, cj, &changed);

      assert(changed == 2); /* one drop + one demote (escalate + unmatched ignored) */
      assert(adj && strstr(adj, "Chair adjudication"));
      assert(strstr(adj, "DROP") && strstr(adj, "guarded by the outer lock"));
      assert(strstr(adj, "DEMOTE") && strstr(adj, "cosmetic"));

      /* dropped item gone from items, present in rejected */
      assert(!has_item(r, "over-flagged race"));
      assert(r->rejected_count == 1 && strcmp(r->rejected[0].summary, "over-flagged race") == 0);
      assert(strcmp(r->rejected_reason[0], "chair-refuted") == 0);

      /* demoted item lowered */
      assert(has_item(r, "real but not blocking"));
      assert(strcmp(sev_of(r, "real but not blocking"), "suggestion") == 0);

      /* escalation ignored: nit stays nit (chair can never raise severity) */
      assert(strcmp(sev_of(r, "typo"), "nit") == 0);

      /* the chair cannot add a finding */
      assert(!has_item(r, "a finding that does not exist"));

      /* untouched item kept as-is */
      assert(has_item(r, "keep me") && strcmp(sev_of(r, "keep me"), "suggestion") == 0);
      assert(r->item_count == 3); /* 4 - 1 dropped */

      free(adj);
      free(r);
   }

   /* --- a demote/drop WITHOUT a rationale is ignored (the survivor is kept) --- */
   {
      roundtable_result_t *r = calloc(1, sizeof(*r));
      set_item(&r->items[0], "blocking", "drop me but no reason");
      set_item(&r->items[1], "blocking", "demote me but no reason");
      r->item_count = 2;
      const char *cj = "{\"verdicts\":["
                       "{\"summary\":\"drop me but no reason\",\"verdict\":\"drop\"},"
                       "{\"summary\":\"demote me but no reason\",\"verdict\":\"demote\","
                       "\"new_severity\":\"nit\"}]}";
      int changed = 0;
      char *adj = roundtable_chair_apply(r, cj, &changed);
      assert(adj == NULL && changed == 0); /* nothing applied without rationale */
      assert(r->item_count == 2 && r->rejected_count == 0);
      assert(strcmp(sev_of(r, "demote me but no reason"), "blocking") == 0);
      free(r);
   }

   /* --- unparseable chair output: no-op, out untouched, returns NULL --- */
   {
      roundtable_result_t *r = calloc(1, sizeof(*r));
      set_item(&r->items[0], "blocking", "x");
      r->item_count = 1;
      int changed = 7;
      char *adj = roundtable_chair_apply(r, "the model returned prose, not json", &changed);
      assert(adj == NULL && changed == 0);
      assert(r->item_count == 1 && strcmp(r->items[0].severity, "blocking") == 0);
      free(r);

      /* no changes (all keep) also yields NULL, nothing appended */
      r = calloc(1, sizeof(*r));
      set_item(&r->items[0], "blocking", "y");
      r->item_count = 1;
      adj = roundtable_chair_apply(r, "{\"verdicts\":[]}", &changed);
      assert(adj == NULL && changed == 0 && r->item_count == 1);
      free(r);
   }

   printf("all tests passed\n");
   return 0;
}
