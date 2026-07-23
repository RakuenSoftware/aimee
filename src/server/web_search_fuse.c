/* web_search_fuse.c: see web_search_fuse.h. */

#include "web_search_fuse.h"

#include "kb_rrf.h"
#include "util.h"
#include "web_page_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap on distinct URLs we will track while fusing. Engine lists are bounded by
 * WEB_SEARCH_MAX_RESULTS each, so this cannot be reached by valid input; it
 * exists so a future caller cannot walk off the arrays. */
#define FUSE_MAX_CANDIDATES (WEB_SEARCH_MAX_ENGINES * WEB_SEARCH_MAX_RESULTS)

int web_search_dedup_key(const char *url, char *out, size_t out_len)
{
   if (!url || !out || out_len == 0)
      return -1;
   return db1_web_page_canonical_url(url, out, out_len);
}

/* One distinct page, in first-seen order. */
typedef struct
{
   char key[2304];
   int list;  /* list it was first seen in */
   int index; /* its position within that list */
} fuse_cand_t;

/* Find `key`, or append it. Returns its index, or -1 when full. */
static int cand_intern(fuse_cand_t *cands, int *n, const char *key, int list, int index)
{
   for (int i = 0; i < *n; i++)
      if (strcmp(cands[i].key, key) == 0)
         return i;
   if (*n >= FUSE_MAX_CANDIDATES)
      return -1;
   snprintf(cands[*n].key, sizeof(cands[*n].key), "%s", key);
   cands[*n].list = list;
   cands[*n].index = index;
   return (*n)++;
}

int web_search_fuse(const web_search_result_t *const *lists, const int *counts, int n_lists,
                    web_search_result_t *out, int max)
{
   if (!lists || !counts || n_lists <= 0 || n_lists > WEB_SEARCH_MAX_ENGINES || !out || max <= 0)
      return -1;

   fuse_cand_t *cands = calloc(FUSE_MAX_CANDIDATES, sizeof(*cands));
   kb_rrf_item_t *items = calloc((size_t)n_lists * WEB_SEARCH_MAX_RESULTS, sizeof(*items));
   kb_rrf_signal_t signals[WEB_SEARCH_MAX_ENGINES];
   kb_rrf_result_t *fused = calloc(FUSE_MAX_CANDIDATES, sizeof(*fused));
   if (!cands || !items || !fused)
   {
      free(cands);
      free(items);
      free(fused);
      return -1;
   }

   int ncand = 0;
   int nsig = 0;
   for (int l = 0; l < n_lists; l++)
   {
      /* A NULL or empty list is an engine that failed or matched nothing. It is
       * skipped rather than treated as evidence: a dead engine must not sink the
       * results of a live one. */
      if (!lists[l] || counts[l] <= 0)
         continue;
      kb_rrf_item_t *slot = items + (size_t)nsig * WEB_SEARCH_MAX_RESULTS;
      int nitems = 0;
      int cap = counts[l] < WEB_SEARCH_MAX_RESULTS ? counts[l] : WEB_SEARCH_MAX_RESULTS;
      for (int i = 0; i < cap; i++)
      {
         const char *url = lists[l][i].url;
         if (!url || !url[0])
            continue;
         char key[2304];
         if (web_search_dedup_key(url, key, sizeof(key)) != 0)
         {
            /* Unparseable: give it its own identity rather than letting every
             * such URL collapse into one shared bucket. */
            snprintf(key, sizeof(key), "raw:%d:%d:%s", l, i, url);
         }
         int c = cand_intern(cands, &ncand, key, l, i);
         if (c < 0)
            continue;
         /* The RRF id is the candidate INDEX, not the URL: kb_rrf_item_t.id is
          * char[256] and a normalised URL can exceed that, so using the URL
          * would truncate and silently merge distinct pages that share a long
          * prefix. Zero-padded so kb_rrf's documented `id asc` final tie-break
          * orders by first-seen -- the earliest engine's better-ranked hit --
          * rather than lexicographically ("10" < "2"). */
         snprintf(slot[nitems].id, sizeof(slot[nitems].id), "%04d", c);
         slot[nitems].structural_weight = 0;
         nitems++;
      }
      if (nitems == 0)
         continue;
      signals[nsig].items = slot;
      signals[nsig].count = nitems;
      signals[nsig].weight = 1.0; /* engines are peers; none is known better */
      signals[nsig].label = NULL;
      nsig++;
   }

   int written = 0;
   if (nsig > 0)
   {
      int nf = kb_rrf_fuse(signals, nsig, KB_RRF_DEFAULT_K, fused, FUSE_MAX_CANDIDATES);
      for (int i = 0; i < nf && written < max; i++)
      {
         int c = atoi(fused[i].id);
         if (c < 0 || c >= ncand)
            continue;
         const web_search_result_t *src = &lists[cands[c].list][cands[c].index];
         out[written].title = src->title ? safe_strdup(src->title) : NULL;
         out[written].url = src->url ? safe_strdup(src->url) : NULL;
         out[written].snippet = src->snippet ? safe_strdup(src->snippet) : NULL;
         written++;
      }
   }

   free(cands);
   free(items);
   free(fused);
   return written;
}
