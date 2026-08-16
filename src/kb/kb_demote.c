/* kb_demote.c: outcome-driven demotion maintenance runner.
 * See docs/proposals/done/outcome-driven-demotion-and-poison-resilience.md */

#include "kb_demote.h"
#include "aimee.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/demotion.h"
#include "modules/db2/c/memory_promotion.h"
#include "modules/db2/c/memory_query.h"
#include "log.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMOTE_MAX_CANDIDATES 4096
#define DEMOTE_PERCENTILES    5

/* Score accumulator per memory class for profile fitting. */
typedef struct
{
   char kind[64];
   double scores[DEMOTE_MAX_CANDIDATES];
   int n;
} class_scores_t;

static int compare_double(const void *a, const void *b)
{
   double da = *(const double *)a;
   double db = *(const double *)b;
   return (da > db) - (da < db);
}

static double percentile(double *sorted, int n, double p)
{
   if (n <= 0)
      return 0.0;
   double idx = p * (n - 1);
   int lo = (int)idx;
   int hi = lo + 1;
   if (hi >= n)
      return sorted[n - 1];
   double frac = idx - lo;
   return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

/* Build a profile JSON payload from a set of demotion scores. */
static void build_profile_payload(const char *kind, const double *scores, int n, int n_candidates,
                                  char *buf, size_t len)
{
   double sorted[DEMOTE_MAX_CANDIDATES];
   int m = n < DEMOTE_MAX_CANDIDATES ? n : DEMOTE_MAX_CANDIDATES;
   for (int i = 0; i < m; i++)
      sorted[i] = scores[i];
   qsort(sorted, (size_t)m, sizeof(double), compare_double);

   double p10 = percentile(sorted, m, 0.10);
   double p25 = percentile(sorted, m, 0.25);
   double p50 = percentile(sorted, m, 0.50);
   double p75 = percentile(sorted, m, 0.75);
   double p90 = percentile(sorted, m, 0.90);

   snprintf(buf, len,
            "{\"memory_class\":\"%s\",\"n_rows_scored\":%d,\"n_candidates\":%d,"
            "\"score_percentiles\":{\"p10\":%.4f,\"p25\":%.4f,\"p50\":%.4f,"
            "\"p75\":%.4f,\"p90\":%.4f}}",
            kind, m, n_candidates, p10, p25, p50, p75, p90);
}

int kb_demote_run(void)
{
   int demotion_enabled = config_demotion_enabled();
   if (demotion_enabled == 0)
      return 0;

   /* Read once: these feed a per-candidate loop, and one pinned read each is the point. */
   int n_min = config_demotion_n_min();
   int window = config_demotion_window();
   double half_life_days = config_demotion_half_life_days();

   db2_demotion_candidate_t candidates[DEMOTE_MAX_CANDIDATES];
   int n_candidates = db2_demotion_candidates(n_min, candidates, DEMOTE_MAX_CANDIDATES);
   if (n_candidates <= 0)
      return 0;

   /* Collect demotion scores and the memory kind for each candidate. */
   typedef struct
   {
      int64_t row_id;
      char kind[64];
      double score;
   } scored_row_t;

   scored_row_t *rows = calloc((size_t)n_candidates, sizeof(scored_row_t));
   if (!rows)
      return -1;

   int n_scored = 0;
   for (int i = 0; i < n_candidates; i++)
   {
      double score = db2_demotion_score(candidates[i].row_id, window, half_life_days, n_min);
      if (isnan(score))
         continue;

      /* Look up the memory kind for this row. */
      memory_t mem;
      memset(&mem, 0, sizeof(mem));
      int rc = db2_memory_get(candidates[i].row_id, &mem);
      if (rc != 0 || !mem.kind[0])
         continue;

      rows[n_scored].row_id = candidates[i].row_id;
      snprintf(rows[n_scored].kind, sizeof(rows[n_scored].kind), "%s", mem.kind);
      rows[n_scored].score = score;
      n_scored++;

      aimee_log(LOG_DEBUG, "demotion", "row=%lld kind=%s score=%.4f",
                (long long)candidates[i].row_id, mem.kind, score);
   }

   /* Group scores by memory class and write one demotion_profile per class. */
   int written = 0;
   for (int i = 0; i < n_scored; i++)
   {
      const char *kind = rows[i].kind;
      if (!kind[0])
         continue;

      /* Check if we already processed this kind. */
      int already = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(rows[j].kind, kind) == 0)
         {
            already = 1;
            break;
         }
      }
      if (already)
         continue;

      /* Collect scores for this kind. */
      double kind_scores[DEMOTE_MAX_CANDIDATES];
      int n_kind = 0;
      for (int j = i; j < n_scored && n_kind < DEMOTE_MAX_CANDIDATES; j++)
      {
         if (strcmp(rows[j].kind, kind) == 0)
            kind_scores[n_kind++] = rows[j].score;
      }

      char payload[1024];
      build_profile_payload(kind, kind_scores, n_kind, n_candidates, payload, sizeof(payload));

      char art_id[64];
      int wrc = db2_demotion_profile_write(kind, "global", "", payload, art_id, sizeof(art_id));
      if (wrc == 0)
      {
         written++;
         aimee_log(LOG_DEBUG, "demotion", "wrote demotion_profile kind=%s id=%s n_scored=%d", kind,
                   art_id, n_kind);
      }
      else
      {
         aimee_log(LOG_DEBUG, "demotion", "failed to write demotion_profile kind=%s", kind);
      }
   }

   /* Phase 2: live demotion — apply demotions for rows below class p10.
    * Only fires at demotion_enabled >= 2. */
   int demoted = 0;
   if (demotion_enabled >= 2)
   {
      for (int i = 0; i < n_scored; i++)
      {
         const char *kind = rows[i].kind;
         if (!kind[0])
            continue;

         int already = 0;
         for (int j = 0; j < i; j++)
         {
            if (strcmp(rows[j].kind, kind) == 0)
            {
               already = 1;
               break;
            }
         }
         if (already)
            continue;

         char pbuf[1024];
         if (db2_demotion_profile_read(kind, "global", "", pbuf, sizeof(pbuf)) != 0)
            continue;
         cJSON *pj = cJSON_ParseWithLength(pbuf, strlen(pbuf));
         if (!pj)
            continue;
         cJSON *percs = cJSON_GetObjectItemCaseSensitive(pj, "score_percentiles");
         cJSON *p10j = percs ? cJSON_GetObjectItemCaseSensitive(percs, "p10") : NULL;
         double p10 = cJSON_IsNumber(p10j) ? p10j->valuedouble : 0.0;
         cJSON_Delete(pj);

         for (int j = i; j < n_scored; j++)
         {
            if (strcmp(rows[j].kind, kind) != 0)
               continue;
            if (rows[j].score >= p10)
               continue;

            char art_id[64];
            char scope_id_str[32];
            char payload[256];
            snprintf(scope_id_str, sizeof(scope_id_str), "%lld", (long long)rows[j].row_id);
            snprintf(payload, sizeof(payload),
                     "{\"row_id\":%lld,\"kind\":\"%s\",\"score\":%.4f,\"p10\":%.4f}",
                     (long long)rows[j].row_id, kind, rows[j].score, p10);
            db2_artifact_gen_id(art_id, sizeof(art_id));
            db2_artifact_write(art_id, "demotion_action", "demoted", "memory", scope_id_str, "",
                               0.0, payload);
            db2_memory_promotion_demote_id(rows[j].row_id);
            demoted++;
            aimee_log(LOG_DEBUG, "demotion", "live demote row=%lld kind=%s score=%.4f p10=%.4f",
                      (long long)rows[j].row_id, kind, rows[j].score, p10);
         }
      }
   }

   free(rows);
   return written + demoted;
}
