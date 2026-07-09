/* retrieval_outcome_bridge.c: dogfood-autolabel -> retrieval outcome bridge.
 * See retrieval_outcome_bridge.h and
 * docs/proposals/pending/kb-hybrid-outcome-wiring.md. */

#include "retrieval_outcome_bridge.h"
#include "config.h"
#include "kb_client.h"
#include "db2/demotion.h" /* DEMOTION_VERDICT_* */

#include <string.h>

#define ROB_MAX_IDS 8

/* Process-global single-turn memory, mirroring dogfood's g_last_record_id. This
 * carries the prior turn's surfaced rows across to the next turn's autolabel. On
 * a personal instance turns are effectively serialized; a benign cross-session
 * race can only drop/mis-slot a weak label, never corrupt the answer. */
typedef struct
{
   char event_id[64];
   int64_t ids[ROB_MAX_IDS];
   int n;
} rob_pending_t;

static rob_pending_t g_mem;  /* memory surface  -> retrieval_attribution */
static rob_pending_t g_rank; /* ranker surface  -> ranker_outcome        */

static void rob_fill(rob_pending_t *p, const char *event_id, const int64_t *ids, int n)
{
   if (!event_id || !event_id[0] || n <= 0 || !ids)
   {
      p->n = 0;
      return;
   }
   snprintf(p->event_id, sizeof(p->event_id), "%s", event_id);
   int k = n < ROB_MAX_IDS ? n : ROB_MAX_IDS;
   for (int i = 0; i < k; i++)
      p->ids[i] = ids[i];
   p->n = k;
}

void retrieval_outcome_bridge_note(const char *surface, const char *event_id, const int64_t *ids,
                                   int n)
{
   if (!surface)
      return;
   if (strcmp(surface, "ranker") == 0)
      rob_fill(&g_rank, event_id, ids, n);
   else if (strcmp(surface, "memory") == 0)
      rob_fill(&g_mem, event_id, ids, n);
}

void retrieval_outcome_bridge_reset(void)
{
   g_mem.n = 0;
   g_rank.n = 0;
}

void retrieval_outcome_bridge_on_autolabel(int is_continuation, int is_repair)
{
   /* Nothing pending: cheap exit before touching config. */
   if (g_mem.n <= 0 && g_rank.n <= 0)
      return;

   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.learning_implicit_retrieval_outcome)
   {
      retrieval_outcome_bridge_reset();
      return;
   }

   /* Continuation = the answer built on these rows stuck -> accepted.
    * Repair = the user corrected it -> corrected (a negative demotion verdict).
    * Anything else carries no usable signal; we still consume (drop) so the note
    * cannot leak onto a later, unrelated turn. */
   const char *verdict = NULL;
   if (is_continuation)
      verdict = DEMOTION_VERDICT_ACCEPTED;
   else if (is_repair)
      verdict = DEMOTION_VERDICT_CORRECTED;

   if (verdict)
   {
      if (g_mem.n > 0)
         (void)kb_client_record_retrieval_outcome("memory", g_mem.event_id, g_mem.ids, g_mem.n,
                                                  verdict);
      if (g_rank.n > 0)
         (void)kb_client_record_retrieval_outcome("ranker", g_rank.event_id, g_rank.ids, g_rank.n,
                                                  verdict);
   }

   retrieval_outcome_bridge_reset();
}
