/* rrf.c: see headers/kb_rrf.h. Reciprocal Rank Fusion over ranked signal lists. */
#include "kb_rrf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Accumulator for one distinct candidate while fusing. */
typedef struct
{
   char id[256];
   double score;
   int structural_weight;
   int signal_hits;
   double trust; /* §3 earned trust; a TIE-BREAK only (0 when no lessons/lookup) */
} rrf_acc_t;

/* Sort key: score desc; then structural_weight desc; then id asc. Scores are
 * compared EXACTLY (no epsilon): an epsilon band would break qsort's strict-weak-
 * ordering contract (cmp(a,b)==0 ∧ cmp(b,c)==0 but cmp(a,c)≠0 when scores cluster
 * within the band → undefined behavior). A genuine tie produces bit-identical
 * doubles here — every contribution is weight/(k+rank) accumulated in the same
 * order across runs — so true ties fall through deterministically to the
 * structural-weight and id tie-breaks; near-but-not-equal scores order by score. */
static int rrf_cmp(const void *a, const void *b)
{
   const rrf_acc_t *x = (const rrf_acc_t *)a;
   const rrf_acc_t *y = (const rrf_acc_t *)b;
   if (x->score > y->score)
      return -1;
   if (x->score < y->score)
      return 1;
   /* higher structural trust first; branchless, overflow-safe (no int subtraction) */
   if (x->structural_weight != y->structural_weight)
      return (x->structural_weight < y->structural_weight) -
             (x->structural_weight > y->structural_weight);
   /* §3 earned-trust tie-break: only reached when score AND structural_weight are
    * exactly equal, so it can never move a candidate across a real score gap.
    * Higher trust first. The branchless form (matching the score/structural idioms)
    * returns 0 when either side is NaN — so a NaN trust degrades to the id tie-break
    * instead of violating qsort's strict-weak-ordering. */
   if (x->trust < y->trust)
      return 1;
   if (x->trust > y->trust)
      return -1;
   return strcmp(x->id, y->id);
}

int kb_rrf_fuse_trust(const kb_rrf_signal_t *signals, int n, double k, const kb_rrf_trust_t *trust,
                      int n_trust, kb_rrf_result_t *out, int max)
{
   /* isfinite guards: NaN slips through `k <= 0.0` (every NaN comparison is false),
    * which would propagate NaN into out[].score and corrupt the ordering. */
   if (!signals || n < 0 || !isfinite(k) || k <= 0.0 || !out || max <= 0)
      return -1;

   /* Upper bound on distinct candidates = sum of all signal list lengths, in a
    * 64-bit accumulator so a pathological caller can't overflow a signed int into
    * a negative (then huge) allocation size. */
   long long total = 0;
   for (int s = 0; s < n; s++)
      if (signals[s].items && signals[s].count > 0 && isfinite(signals[s].weight) &&
          signals[s].weight > 0.0)
         total += signals[s].count;
   if (total <= 0)
      return 0;

   rrf_acc_t *acc = calloc((size_t)total, sizeof(*acc));
   if (!acc)
      return -1;
   int nacc = 0;

   for (int s = 0; s < n; s++)
   {
      const kb_rrf_signal_t *sig = &signals[s];
      if (!sig->items || sig->count <= 0 || !isfinite(sig->weight) || sig->weight <= 0.0)
         continue;
      for (int i = 0; i < sig->count; i++)
      {
         const char *id = sig->items[i].id;
         if (!id[0])
            continue;
         /* 1-based rank: the signal's top hit (index 0) has rank 1. */
         double contribution = sig->weight / (k + (double)(i + 1));

         /* Find-or-insert this id in the accumulator (linear; lists are small
          * and capped per signal, so the candidate set is bounded). */
         int found = -1;
         for (int j = 0; j < nacc; j++)
            if (strcmp(acc[j].id, id) == 0)
            {
               found = j;
               break;
            }
         if (found < 0)
         {
            found = nacc++;
            snprintf(acc[found].id, sizeof(acc[found].id), "%s", id);
            acc[found].score = 0.0;
            acc[found].structural_weight = sig->items[i].structural_weight;
            acc[found].signal_hits = 0;
            acc[found].trust = 0.0;
         }
         acc[found].score += contribution;
         acc[found].signal_hits++;
         if (sig->items[i].structural_weight > acc[found].structural_weight)
            acc[found].structural_weight = sig->items[i].structural_weight;
      }
   }

   /* Attach earned trust (tie-break only). Unordered lookup by id; a candidate
    * absent from the lessons artifact keeps trust 0. */
   if (trust && n_trust > 0)
      for (int j = 0; j < nacc; j++)
         for (int t = 0; t < n_trust; t++)
            if (strcmp(acc[j].id, trust[t].id) == 0)
            {
               acc[j].trust = trust[t].trust;
               break;
            }

   qsort(acc, (size_t)nacc, sizeof(*acc), rrf_cmp);

   int w = nacc < max ? nacc : max;
   for (int i = 0; i < w; i++)
   {
      snprintf(out[i].id, sizeof(out[i].id), "%s", acc[i].id);
      out[i].score = acc[i].score;
      out[i].structural_weight = acc[i].structural_weight;
      out[i].signal_hits = acc[i].signal_hits;
   }
   free(acc);
   return w;
}

int kb_rrf_fuse(const kb_rrf_signal_t *signals, int n, double k, kb_rrf_result_t *out, int max)
{
   /* No trust lookup → the tie-break is inert → byte-identical to pre-§3 behavior. */
   return kb_rrf_fuse_trust(signals, n, k, NULL, 0, out, max);
}
