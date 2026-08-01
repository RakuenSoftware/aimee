/* agent_eval_baseline.c: regression baseline I/O for memory retrieval evals.
 *
 * Extracted from agent_eval.c to keep that file under the 2000-line lint cap.
 * All three functions here are the public load / compare / save triple that
 * agent_eval's benchmark drivers call to gate regressions against a frozen
 * set of metrics on disk. They share a tight coupling to the baseline JSON
 * schema and nothing else, which is why they live together. */
#include "aimee.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mem_eval_load_baseline(const char *baseline_path, mem_eval_scores_t *out,
                           double *threshold_pct_out)
{
   if (!baseline_path || !out)
      return -1;

   char *raw = slurp_file_eval(baseline_path);
   if (!raw)
      return -1;

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root)
      return -1;

   memset(out, 0, sizeof(*out));
   if (threshold_pct_out)
      *threshold_pct_out = 5.0; /* default */

   cJSON *j = NULL;

   j = cJSON_GetObjectItem(root, "mrr");
   if (j && cJSON_IsNumber(j))
      out->mrr = j->valuedouble;

   j = cJSON_GetObjectItem(root, "ndcg_5");
   if (j && cJSON_IsNumber(j))
      out->ndcg_5 = j->valuedouble;

   j = cJSON_GetObjectItem(root, "ndcg_10");
   if (j && cJSON_IsNumber(j))
      out->ndcg_10 = j->valuedouble;

   j = cJSON_GetObjectItem(root, "recall_5");
   if (j && cJSON_IsNumber(j))
      out->recall_5 = j->valuedouble;

   j = cJSON_GetObjectItem(root, "recall_10");
   if (j && cJSON_IsNumber(j))
      out->recall_10 = j->valuedouble;

   j = cJSON_GetObjectItem(root, "n_cases");
   if (j && cJSON_IsNumber(j))
      out->n_cases = (int)j->valuedouble;

   if (threshold_pct_out)
   {
      j = cJSON_GetObjectItem(root, "threshold_pct");
      if (j && cJSON_IsNumber(j))
         *threshold_pct_out = j->valuedouble;
   }

   cJSON_Delete(root);
   return 0;
}

int mem_eval_check_regression(const mem_eval_scores_t *scores, const mem_eval_scores_t *baseline,
                              double threshold_pct)
{
   if (!scores || !baseline)
      return 0;

   if (threshold_pct <= 0.0)
      threshold_pct = 5.0;

   int regressed = 0;

#define CHECK_METRIC(name, field)                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (baseline->field > 0.0)                                                                   \
      {                                                                                            \
         double drop_pct = (baseline->field - scores->field) / baseline->field * 100.0;            \
         if (drop_pct > threshold_pct)                                                             \
         {                                                                                         \
            fprintf(stderr,                                                                        \
                    "regression: " name " dropped %.2f%% (%.4f → %.4f, threshold %.1f%%)\n",       \
                    drop_pct, baseline->field, scores->field, threshold_pct);                      \
            regressed = 1;                                                                         \
         }                                                                                         \
      }                                                                                            \
   } while (0)

   CHECK_METRIC("MRR", mrr);
   CHECK_METRIC("NDCG@5", ndcg_5);
   CHECK_METRIC("NDCG@10", ndcg_10);
   CHECK_METRIC("Recall@5", recall_5);
   CHECK_METRIC("Recall@10", recall_10);

#undef CHECK_METRIC

   return regressed;
}

int mem_eval_save_baseline(const char *baseline_path, const mem_eval_scores_t *scores,
                           double threshold_pct)
{
   if (!baseline_path || !scores)
      return -1;

   if (threshold_pct <= 0.0)
      threshold_pct = 5.0;

   FILE *f = fopen(baseline_path, "w");
   if (!f)
      return -1;

   fprintf(
       f,
       "{\n"
       "  \"mrr\": %.6f,\n"
       "  \"ndcg_5\": %.6f,\n"
       "  \"ndcg_10\": %.6f,\n"
       "  \"recall_5\": %.6f,\n"
       "  \"recall_10\": %.6f,\n"
       "  \"n_cases\": %d,\n"
       "  \"threshold_pct\": %.1f,\n"
       "  \"description\": \"Baseline metrics for memory retrieval eval using the golden corpus."
       " Update with `aimee eval memory-retrieval --update-baseline` after intentional"
       " retrieval improvements.\"\n"
       "}\n",
       scores->mrr, scores->ndcg_5, scores->ndcg_10, scores->recall_5, scores->recall_10,
       scores->n_cases, threshold_pct);

   fclose(f);
   return 0;
}
