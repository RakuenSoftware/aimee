#include "../kb_mdl.h"
#include "config.h"
#include "config_learning.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_mdl_score_shorter_wins(void)
{
   /* A concise summary should have lower L(S) than a verbose restatement. */
   const char *verbose = "The quick brown fox jumps over the lazy dog. "
                         "The quick brown fox jumps over the lazy dog. "
                         "The quick brown fox jumps over the lazy dog.";
   const char *concise = "Fox jumps over dog.";
   const char *evidence = "Quick brown fox leaps over a lazy dog lying in the sun.";

   kb_mdl_score_t sv = {0}, sc = {0};
   assert(kb_mdl_score(verbose, evidence, &sv) == 0);
   assert(kb_mdl_score(concise, evidence, &sc) == 0);

   /* Concise must have smaller L(candidate). */
   assert(sc.l_candidate < sv.l_candidate);
   printf("  PASS  test_mdl_score_shorter_wins (concise=%.1f verbose=%.1f)\n", sc.l_candidate,
          sv.l_candidate);
}

static void test_mdl_score_copy_has_low_residual(void)
{
   /* A candidate identical to the evidence has near-zero L(E|S) because
    * ZSTD(S||SEP||E) ≈ ZSTD(S) when S = E (back-reference compression). */
   const char *evidence = "The system uses PostgreSQL as the primary database backend.";
   const char *copy = "The system uses PostgreSQL as the primary database backend.";
   const char *unrelated = "The frontend is built with React and TypeScript for the UI.";

   kb_mdl_score_t sc_copy = {0}, sc_unrel = {0};
   assert(kb_mdl_score(copy, evidence, &sc_copy) == 0);
   assert(kb_mdl_score(unrelated, evidence, &sc_unrel) == 0);

   /* Copy must have lower l_residual than unrelated candidate. */
   assert(sc_copy.l_residual < sc_unrel.l_residual);
   printf("  PASS  test_mdl_score_copy_has_low_residual"
          " (copy_residual=%.1f unrelated_residual=%.1f)\n",
          sc_copy.l_residual, sc_unrel.l_residual);
}

static void test_mdl_score_deterministic(void)
{
   const char *cand = "Deterministic MDL output.";
   const char *evid = "The MDL score must be identical across repeated calls.";

   kb_mdl_score_t a = {0}, b = {0};
   assert(kb_mdl_score(cand, evid, &a) == 0);
   assert(kb_mdl_score(cand, evid, &b) == 0);

   assert(a.l_candidate == b.l_candidate);
   assert(a.l_residual == b.l_residual);
   assert(a.total == b.total);
   printf("  PASS  test_mdl_score_deterministic\n");
}

static void test_mdl_score_null_handling(void)
{
   kb_mdl_score_t s = {0};
   assert(kb_mdl_score(NULL, "ev", &s) == -1);
   assert(kb_mdl_score("cand", NULL, &s) == -1);
   assert(kb_mdl_score("cand", "ev", NULL) == -1);
   printf("  PASS  test_mdl_score_null_handling\n");
}

static void test_mdl_select_picks_winner(void)
{
   const char *evidence = "The cache key is derived from the request path and query string.";
   const char *candidates[] = {
       /* verbose restatement */
       "The cache key is derived from the request path and query string, "
       "which includes all parameters after the question mark.",
       /* concise summary */
       "Cache key = path + query.",
       /* medium */
       "Cache key is derived from request path and query.",
   };
   int n = 3;
   kb_mdl_score_t scores[3];
   memset(scores, 0, sizeof(scores));

   int winner = kb_mdl_select(candidates, n, evidence, scores);
   assert(winner >= 0 && winner < n);
   /* Winner must have rank 1. */
   assert(scores[winner].rank_in_cluster == 1);
   /* Concise candidate (index 1) should win or tie. */
   assert(scores[1].rank_in_cluster <= scores[0].rank_in_cluster);
   printf("  PASS  test_mdl_select_picks_winner (winner=%d)\n", winner);
}

static void test_mdl_select_respects_agreement_cluster(void)
{
   const char *evidence = "The primary database is PostgreSQL 15 running on port 5432.";
   const char *candidates[] = {
       "Primary DB: PostgreSQL 15 on port 5432.",
       "PostgreSQL 15 listens on 5432.",
       "MySQL.",
   };
   const char *clusters[] = {"A", "A", "B"};
   kb_mdl_score_t scores[3];
   memset(scores, 0, sizeof(scores));

   int winner = kb_mdl_select_agreed_cluster(candidates, clusters, 3, evidence, 2, scores);
   assert(winner == 0 || winner == 1);
   assert(strcmp(clusters[winner], "A") == 0);
   assert(scores[winner].rank_in_cluster == 1);
   assert(scores[2].rank_in_cluster == 0);
   printf("  PASS  test_mdl_select_respects_agreement_cluster (winner=%d)\n", winner);
}

static void test_mdl_select_rejects_ambiguous_clusters(void)
{
   const char *evidence = "The cache key includes request path and query string.";
   const char *candidates[] = {
       "Cache key: path plus query.",
       "Path and query compose cache key.",
       "Cache key: request body.",
       "Body forms the cache key.",
   };
   const char *clusters[] = {"A", "A", "B", "B"};
   kb_mdl_score_t scores[4];
   memset(scores, 0, sizeof(scores));

   int winner = kb_mdl_select_agreed_cluster(candidates, clusters, 4, evidence, 2, scores);
   assert(winner == -2);
   for (int i = 0; i < 4; i++)
   {
      assert(scores[i].l_candidate == 0.0);
      assert(scores[i].l_residual == 0.0);
      assert(scores[i].total == 0.0);
      assert(scores[i].rank_in_cluster == 0);
   }
   printf("  PASS  test_mdl_select_rejects_ambiguous_clusters\n");
}

static void test_mdl_config_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_mdl_settings(&cfg, NULL);

   assert(cfg.kb_mdl_tiebreak_enabled == 1);
   assert(fabs(cfg.kb_mdl_bump_drift_alert - 0.30) < 1e-9);
   printf("  PASS  test_mdl_config_defaults\n");
}

static void test_mdl_drift_fires(void)
{
   /* prompt_bump_drift_fires fixture: compact pre-bump vs verbose hedged post-bump.
    * The post-bump candidate is much longer/more expensive to compress relative to
    * the evidence, so drift fraction should exceed 0.30. */
   const char *evidence =
       "Configuration defaults are loaded from YAML. Overrides can be applied via environment "
       "variables or a runtime config patch.";
   const char *pre_candidates[] = {
       "Config: YAML defaults, env-var overrides, runtime patch.",
   };
   const char *post_candidates[] = {
       "It appears that configuration defaults may potentially be loaded from YAML files, with "
       "possible overrides potentially applicable via environment variables or a runtime config "
       "patch, if supported.",
   };
   int rc = kb_mdl_drift_alert(pre_candidates, 1, post_candidates, 1, evidence, 0.30);
   assert(rc == 1);
   printf("  PASS  test_mdl_drift_fires\n");
}

static void test_mdl_drift_stable(void)
{
   /* prompt_bump_stable_no_alert fixture: both pre and post candidates are compact
    * and similar in MDL cost; drift fraction should be below 0.30. */
   const char *evidence = "The embedding model is text-embedding-ada-002.";
   const char *pre_candidates[] = {
       "Embedding model: text-embedding-ada-002.",
   };
   const char *post_candidates[] = {
       "Uses text-embedding-ada-002 for embeddings.",
   };
   int rc = kb_mdl_drift_alert(pre_candidates, 1, post_candidates, 1, evidence, 0.30);
   assert(rc == 0);
   printf("  PASS  test_mdl_drift_stable\n");
}

int main(void)
{
   printf("=== test_kb_mdl ===\n");
   test_mdl_score_shorter_wins();
   test_mdl_score_copy_has_low_residual();
   test_mdl_score_deterministic();
   test_mdl_score_null_handling();
   test_mdl_select_picks_winner();
   test_mdl_select_respects_agreement_cluster();
   test_mdl_select_rejects_ambiguous_clusters();
   test_mdl_config_defaults();
   test_mdl_drift_fires();
   test_mdl_drift_stable();
   printf("All kb_mdl tests passed.\n");
   return 0;
}
