/* kb_mdl.c: Two-part MDL scoring for synthesis candidate selection.
 * See docs/proposals/accepted/mdl-guided-synthesis-selection.md
 *
 * L(S)   = zstd-3 compressed size of candidate.
 * L(E|S) = zstd-3(S || SEP || E) - L(S) - len(SEP).
 * total  = L(S) + L(E|S).   Lower = better.
 */

#include "kb_mdl.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef AIMEE_DISABLE_ZSTD
#include <zstd.h>
#endif

#define MDL_ZSTD_LEVEL 3
/* Separator between candidate and evidence in the joint compression.
 * Must not appear in either string so sep_overhead is exactly len(SEP). */
#define MDL_SEP     "\x00\xff\x00\xff"
#define MDL_SEP_LEN 4

static size_t _compress(const void *src, size_t src_len)
{
   if (!src || src_len == 0)
      return 0;
#ifdef AIMEE_DISABLE_ZSTD
   /* zstd is unavailable on this build (e.g. the MinGW/Windows portable build,
    * which has no libzstd). Fall back to the raw byte length as the size proxy
    * so MDL stays a deterministic, monotonic length-based tie-break. Production
    * builds (Linux/macOS) link real libzstd and use zstd-3 compressed ratios. */
   return src_len;
#else
   size_t bound = ZSTD_compressBound(src_len);
   if (ZSTD_isError(bound))
      return (size_t)-1;
   void *dst = malloc(bound);
   if (!dst)
      return (size_t)-1;
   size_t result = ZSTD_compress(dst, bound, src, src_len, MDL_ZSTD_LEVEL);
   free(dst);
   if (ZSTD_isError(result))
      return (size_t)-1;
   return result;
#endif
}

int kb_mdl_score(const char *candidate, const char *evidence, kb_mdl_score_t *out)
{
   if (!candidate || !evidence || !out)
      return -1;

   size_t clen = strlen(candidate);
   size_t elen = strlen(evidence);

   size_t lc = _compress(candidate, clen);
   if (lc == (size_t)-1)
      return -1;

   /* Build joint buffer: candidate + SEP + evidence. */
   size_t joint_len = clen + MDL_SEP_LEN + elen;
   char *joint = malloc(joint_len);
   if (!joint)
      return -1;
   memcpy(joint, candidate, clen);
   memcpy(joint + clen, MDL_SEP, MDL_SEP_LEN);
   memcpy(joint + clen + MDL_SEP_LEN, evidence, elen);

   size_t lce = _compress(joint, joint_len);
   free(joint);
   if (lce == (size_t)-1)
      return -1;

   /* L(E|S) = joint compression - L(S) - sep_overhead.  Clamp at 0. */
   double l_s = (double)lc;
   double l_ce = (double)lce;
   double l_e_s = l_ce - l_s - (double)MDL_SEP_LEN;
   if (l_e_s < 0.0)
      l_e_s = 0.0;

   out->l_candidate = l_s;
   out->l_residual = l_e_s;
   out->total = l_s + l_e_s;
   out->rank_in_cluster = 1;
   return 0;
}

int kb_mdl_select(const char *const *candidates, int n, const char *evidence,
                  kb_mdl_score_t *scores)
{
   if (!candidates || n <= 0 || n > MDL_MAX_CANDIDATES || !evidence || !scores)
      return -1;

   for (int i = 0; i < n; i++)
   {
      if (kb_mdl_score(candidates[i], evidence, &scores[i]) != 0)
      {
         (void)i; /* compression failure logged by caller if needed */
         scores[i].total = 1e18;
         scores[i].rank_in_cluster = n;
      }
   }

   /* Build index array for ranking without shuffling the original scores. */
   int idx[MDL_MAX_CANDIDATES];
   for (int i = 0; i < n; i++)
      idx[i] = i;

   /* Simple selection sort on idx by scores[idx[i]].total. */
   for (int i = 0; i < n - 1; i++)
   {
      int min_j = i;
      for (int j = i + 1; j < n; j++)
         if (scores[idx[j]].total < scores[idx[min_j]].total)
            min_j = j;
      int tmp = idx[i];
      idx[i] = idx[min_j];
      idx[min_j] = tmp;
   }

   for (int rank = 0; rank < n; rank++)
      scores[idx[rank]].rank_in_cluster = rank + 1;

   return idx[0]; /* index of winner */
}

int kb_mdl_select_agreed_cluster(const char *const *candidates, const char *const *clusters, int n,
                                 const char *evidence, int min_agreement, kb_mdl_score_t *scores)
{
   if (!candidates || !clusters || n <= 0 || n > MDL_MAX_CANDIDATES || !evidence || !scores)
      return -1;
   if (min_agreement <= 0)
      min_agreement = 2;

   for (int i = 0; i < n; i++)
   {
      memset(&scores[i], 0, sizeof(scores[i]));
      if (!candidates[i] || !clusters[i] || !clusters[i][0])
         return -1;
   }

   int eligible_start = -1;
   int eligible_count = 0;
   int eligible_clusters = 0;

   for (int i = 0; i < n; i++)
   {
      int first = 1;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(clusters[i], clusters[j]) == 0)
         {
            first = 0;
            break;
         }
      }
      if (!first)
         continue;

      int count = 0;
      for (int j = 0; j < n; j++)
         if (strcmp(clusters[i], clusters[j]) == 0)
            count++;

      if (count >= min_agreement)
      {
         if (eligible_clusters == 0)
            eligible_start = i;
         eligible_count = count;
         eligible_clusters++;
      }
   }

   if (eligible_clusters == 0)
      return -1;
   if (eligible_clusters > 1)
      return -2;

   const char *cluster_candidates[MDL_MAX_CANDIDATES];
   int original_idx[MDL_MAX_CANDIDATES];
   int k = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(clusters[i], clusters[eligible_start]) != 0)
         continue;
      cluster_candidates[k] = candidates[i];
      original_idx[k] = i;
      k++;
   }
   if (k != eligible_count)
      return -1;

   kb_mdl_score_t cluster_scores[MDL_MAX_CANDIDATES];
   int winner = kb_mdl_select(cluster_candidates, k, evidence, cluster_scores);
   if (winner < 0)
      return -1;

   for (int i = 0; i < k; i++)
      scores[original_idx[i]] = cluster_scores[i];
   return original_idx[winner];
}
