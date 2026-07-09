/* retrieval_outcome_bridge.c: dogfood-autolabel -> retrieval outcome bridge.
 * See retrieval_outcome_bridge.h and
 * docs/proposals/pending/kb-hybrid-outcome-wiring.md. */

#include "retrieval_outcome_bridge.h"
#include "config.h"
#include "kb_client.h"
#include "db2/demotion.h" /* DEMOTION_VERDICT_* */

#include <ctype.h>
#include <string.h>

#define ROB_MAX_IDS     8
#define ROB_SNIPPET_CAP 256
/* A snippet counts as "used" when at least this fraction of its distinctive
 * content tokens appear in the answer (and at least one token matches). */
#define ROB_OVERLAP_FRACTION 0.34
#define ROB_MIN_TOKEN_LEN    4 /* skip short/stopword-ish tokens */

/* Process-global single-turn memory, mirroring dogfood's g_last_record_id. This
 * carries the prior turn's surfaced rows across to the next turn's autolabel. On
 * a personal instance turns are effectively serialized; a benign cross-session
 * race can only drop/mis-slot a weak label, never corrupt the answer. */
typedef struct
{
   char event_id[64];
   int64_t ids[ROB_MAX_IDS];
   char snippets[ROB_MAX_IDS][ROB_SNIPPET_CAP];
   int has_snippets;
   int n;
} rob_pending_t;

static rob_pending_t g_mem;  /* memory surface  -> retrieval_attribution */
static rob_pending_t g_rank; /* ranker surface  -> ranker_outcome        */

static void rob_fill(rob_pending_t *p, const char *event_id, const int64_t *ids,
                     const char *const *snippets, int n)
{
   if (!event_id || !event_id[0] || n <= 0 || !ids)
   {
      p->n = 0;
      p->has_snippets = 0;
      return;
   }
   snprintf(p->event_id, sizeof(p->event_id), "%s", event_id);
   int k = n < ROB_MAX_IDS ? n : ROB_MAX_IDS;
   int any_snip = 0;
   for (int i = 0; i < k; i++)
   {
      p->ids[i] = ids[i];
      if (snippets && snippets[i] && snippets[i][0])
      {
         snprintf(p->snippets[i], sizeof(p->snippets[i]), "%s", snippets[i]);
         any_snip = 1;
      }
      else
      {
         p->snippets[i][0] = '\0';
      }
   }
   p->n = k;
   p->has_snippets = any_snip;
}

void retrieval_outcome_bridge_note(const char *surface, const char *event_id, const int64_t *ids,
                                   const char *const *snippets, int n)
{
   if (!surface)
      return;
   if (strcmp(surface, "ranker") == 0)
      rob_fill(&g_rank, event_id, ids, snippets, n);
   else if (strcmp(surface, "memory") == 0)
      rob_fill(&g_mem, event_id, ids, snippets, n);
}

void retrieval_outcome_bridge_reset(void)
{
   g_mem.n = 0;
   g_mem.has_snippets = 0;
   g_rank.n = 0;
   g_rank.has_snippets = 0;
}

/* Lowercase a copy of s into buf (truncating), for case-insensitive matching. */
static void rob_lower(const char *s, char *buf, size_t cap)
{
   size_t i = 0;
   for (; s && s[i] && i + 1 < cap; i++)
      buf[i] = (char)tolower((unsigned char)s[i]);
   buf[i] = '\0';
}

int retrieval_outcome_overlap_used(const char *answer, const char *snippet)
{
   if (!answer || !answer[0] || !snippet || !snippet[0])
      return 0;

   char ans[8192];
   rob_lower(answer, ans, sizeof(ans));

   char snip[ROB_SNIPPET_CAP];
   rob_lower(snippet, snip, sizeof(snip));

   int total = 0, matched = 0;
   char tok[64];
   size_t tl = 0;
   /* Walk snippet tokens (alnum runs); a token "matches" if it occurs in answer. */
   for (size_t i = 0;; i++)
   {
      char c = snip[i];
      int isword = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (isword && tl + 1 < sizeof(tok))
      {
         tok[tl++] = c;
      }
      else
      {
         if (tl >= (size_t)ROB_MIN_TOKEN_LEN)
         {
            tok[tl] = '\0';
            total++;
            if (strstr(ans, tok))
               matched++;
         }
         tl = 0;
         if (!c)
            break;
      }
   }
   if (total == 0)
      return 0;
   return ((double)matched / (double)total) >= ROB_OVERLAP_FRACTION ? 1 : 0;
}

/* Attribute one surface's pending note. Partition the surfaced ids by verdict and
 * write each verdict-group as one batch. Rules:
 *   continuation (answer good):
 *     - per-doc available: used -> accepted, unused -> corrected (didn't help the
 *       good answer) — this is the within-query contrast.
 *     - else: all -> accepted (flat).
 *   repair (answer bad):
 *     - per-doc available: used -> corrected (fed the bad answer), unused dropped.
 *     - else: all -> corrected (flat). */
static void rob_attribute(rob_pending_t *p, const char *surface, const char *prior_answer,
                          int is_continuation, int is_repair)
{
   if (p->n <= 0)
      return;
   if (!is_continuation && !is_repair)
      return; /* no turn signal */

   int per_doc = (prior_answer && prior_answer[0] && p->has_snippets);

   int64_t acc[ROB_MAX_IDS];
   int n_acc = 0;
   int64_t neg[ROB_MAX_IDS];
   int n_neg = 0;

   for (int i = 0; i < p->n; i++)
   {
      int used = per_doc ? retrieval_outcome_overlap_used(prior_answer, p->snippets[i]) : -1;
      if (is_continuation)
      {
         if (used == -1)
            acc[n_acc++] = p->ids[i]; /* flat: all accepted */
         else if (used)
            acc[n_acc++] = p->ids[i];
         else
            neg[n_neg++] = p->ids[i];
      }
      else /* repair */
      {
         if (used == -1)
            neg[n_neg++] = p->ids[i]; /* flat: all corrected */
         else if (used)
            neg[n_neg++] = p->ids[i];
         /* repair + unused -> no signal, dropped */
      }
   }

   if (n_acc > 0)
      (void)kb_client_record_retrieval_outcome(surface, p->event_id, acc, n_acc,
                                               DEMOTION_VERDICT_ACCEPTED);
   if (n_neg > 0)
      (void)kb_client_record_retrieval_outcome(surface, p->event_id, neg, n_neg,
                                               DEMOTION_VERDICT_CORRECTED);
}

void retrieval_outcome_bridge_on_autolabel(const char *prior_answer, int is_continuation,
                                           int is_repair)
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

   rob_attribute(&g_mem, "memory", prior_answer, is_continuation, is_repair);
   rob_attribute(&g_rank, "ranker", prior_answer, is_continuation, is_repair);

   retrieval_outcome_bridge_reset();
}
