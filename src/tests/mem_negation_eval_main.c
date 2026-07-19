/* mem_negation_eval_main.c: standalone libpq memory-negation eval driver.
 *
 * Unlike unit-test-memory-retrieval-eval (sqlite-shim), this links the real
 * libpq DB2 layer, so mem_eval_load_corpus opens an ISOLATED throwaway schema in
 * a DISPOSABLE Postgres (via db2_eval_open_temp_store_pg, gated on
 * AIMEE_DB2_EVAL_URL) and exercises Postgres-only retrieval features
 * (memory_negation_fts_tsv) that the shim cannot. Single-threaded, one-shot.
 *
 * Usage: AIMEE_DB2_EVAL_URL=postgres://... aimee-negation-eval --corpus PATH
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_eval.h"

int main(int argc, char **argv)
{
   const char *corpus = NULL;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
         corpus = argv[++i];
      else
      {
         fprintf(stderr, "usage: aimee-negation-eval --corpus PATH\n");
         return 2;
      }
   }
   if (!corpus)
   {
      fprintf(stderr, "usage: aimee-negation-eval --corpus PATH\n");
      return 2;
   }

   static mem_eval_case_t cases[MEM_CORPUS_MAX_CASES];
   int n = mem_eval_load_corpus(corpus, cases, MEM_CORPUS_MAX_CASES);
   if (n <= 0)
   {
      fprintf(stderr, "FAIL: corpus load failed for %s\n", corpus);
      return 1;
   }

   mem_eval_scores_t scores;
   mem_eval_latency_t latency;
   int rc = mem_eval_run_with_latency(cases, n, &scores, &latency);
   mem_eval_close_temp_db();
   if (rc != 0)
   {
      fprintf(stderr, "FAIL: eval failed for %s\n", corpus);
      return 1;
   }

   printf("Memory negation eval: cases=%d mrr=%.6f ndcg@5=%.6f recall@5=%.6f recall@10=%.6f "
          "p95=%.2fms\n",
          scores.n_cases, scores.mrr, scores.ndcg_5, scores.recall_5, scores.recall_10,
          latency.p95_ms);
   return 0;
}
