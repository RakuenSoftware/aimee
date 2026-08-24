/* curiosity_resolve.c: draining the curiosity backlog on demand (S4).
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include "curiosity_resolve.h"

#include "log.h"
#include "modules/db2/c/curiosity.h"

#include <stddef.h>
#include <string.h>

/* Open items pulled per pass before the budget is applied. The store returns
 * newest-first; the budget then bounds how many are probed. */
#define CURIOSITY_RESOLVE_MAX_FETCH 256

static curiosity_probe_fn g_probe;

void curiosity_resolve_register_probe(curiosity_probe_fn probe)
{
   g_probe = probe;
}

/* Gap types this pass can honestly close.
 *
 * Both ask "is there support for this?", which a coverage probe answers.
 * `contradiction` and `stale_fact` ask which of two claims is right — closing
 * those on a coverage signal would silently pick a winner, so they are left
 * for a judgement that has actually been made. */
static int gap_is_resolvable_by_coverage(const char *gap_type)
{
   return strcmp(gap_type, CURIOSITY_GAP_UNVERIFIED_ASSUMPTION) == 0 ||
          strcmp(gap_type, CURIOSITY_GAP_WEAK_COVERAGE) == 0;
}

int curiosity_resolve_pass(int budget, curiosity_resolve_stats_t *out)
{
   curiosity_resolve_stats_t local;
   curiosity_resolve_stats_t *stats = out ? out : &local;
   memset(stats, 0, sizeof(*stats));

   if (budget <= 0)
      budget = CURIOSITY_RESOLVE_DEFAULT_BUDGET;
   stats->budget = budget;

   if (!g_probe)
   {
      /* No installed probe means no way to tell whether a gap still stands.
       * Resolving on that basis would empty the backlog by assertion. */
      stats->no_probe = 1;
      LOG_INFO("curiosity", "resolve: no evidence probe installed; nothing was closed");
      return 0;
   }

   static curiosity_item_t items[CURIOSITY_RESOLVE_MAX_FETCH];
   int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, items, CURIOSITY_RESOLVE_MAX_FETCH);
   if (n < 0)
      return -1;

   for (int i = 0; i < n && stats->considered < budget; i++)
   {
      if (!gap_is_resolvable_by_coverage(items[i].gap_type))
      {
         stats->skipped++;
         continue;
      }
      stats->considered++;

      const char *subject =
          items[i].target_entity[0] ? items[i].target_entity : items[i].target_topic;
      if (!subject || !subject[0])
      {
         /* An item with nothing to look up cannot be answered, and must not be
          * closed for lack of a question. */
         stats->unknown++;
         continue;
      }

      curiosity_evidence_t verdict = g_probe(items[i].gap_type, subject, items[i].evidence);
      if (verdict == CURIOSITY_EVIDENCE_FOUND)
      {
         if (db2_curiosity_update_state(items[i].id, CURIOSITY_STATE_RESOLVED) == 0)
            stats->resolved++;
         else
            stats->unknown++; /* the write failed; the gap is still open */
      }
      else if (verdict == CURIOSITY_EVIDENCE_NONE)
         stats->still_open++;
      else
         stats->unknown++;
   }

   LOG_INFO("curiosity",
            "resolve: %d considered (budget %d), %d resolved, %d still open, %d undecided,"
            " %d skipped as needing a judgement",
            stats->considered, stats->budget, stats->resolved, stats->still_open, stats->unknown,
            stats->skipped);
   return stats->resolved;
}
