/* kb_features.c: KB-side feature computation for the ranking substrate.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */

#include "kb_features.h"
#include "modules/db2/c/feature_rows.h"

#include <stdio.h>

int kb_features_upsert_with_sketch(int64_t doc_id, double lex_score, double dense_score,
                                   double age_days, const kb_sketch_features_t *sketch)
{
   char subject_id[32];
   snprintf(subject_id, sizeof(subject_id), "%lld", (long long)doc_id);

   double recency = (age_days > 0.0) ? 1.0 / (1.0 + age_days / 30.0) : 1.0;
   double sketch_frequency = sketch ? sketch->frequency_kind_scope : 0.0;
   double sketch_distinct = sketch ? sketch->distinct_sources_hll : 0.0;

   char features[256];
   snprintf(features, sizeof(features),
            "{\"lex.cos\":%.6f,\"dense.cos\":%.6f,"
            "\"temp.age_days\":%.2f,\"temp.recency\":%.6f,"
            "\"sketch.frequency_kind_scope\":%.6f,"
            "\"sketch.distinct_sources_hll\":%.6f}",
            lex_score, dense_score, age_days > 0.0 ? age_days : 0.0, recency, sketch_frequency,
            sketch_distinct);

   return db2_feature_row_upsert(subject_id, "kb_document", "", "", KB_FEATURE_SET_VERSION,
                                 features, NULL);
}

int kb_features_upsert(int64_t doc_id, double lex_score, double dense_score, double age_days)
{
   return kb_features_upsert_with_sketch(doc_id, lex_score, dense_score, age_days, NULL);
}

int kb_features_read(int64_t doc_id, char *buf, size_t len)
{
   char subject_id[32];
   snprintf(subject_id, sizeof(subject_id), "%lld", (long long)doc_id);
   return db2_feature_row_read(subject_id, "kb_document", KB_FEATURE_SET_VERSION, buf, len);
}

int kb_features_upsert_synthesis_mdl(const char *artifact_id, const kb_mdl_score_t *score)
{
   if (!artifact_id || !score)
      return -1;

   char features[256];
   snprintf(features, sizeof(features),
            "{\"mdl.l_candidate\":%.2f,\"mdl.l_residual\":%.2f,"
            "\"mdl.total\":%.2f,\"mdl.rank_in_cluster\":%d}",
            score->l_candidate, score->l_residual, score->total, score->rank_in_cluster);

   return db2_feature_row_upsert(artifact_id, "synthesis_candidate", "", "", KB_FEATURE_SET_VERSION,
                                 features, NULL);
}
