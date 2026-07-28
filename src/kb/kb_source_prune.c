/* kb_source_prune.c: bounded, crash-resumable source-generation garbage collection. */
#include "kb_source_prune.h"

#include "db2/kb_runtime_state.h"
#include "db2/kb_service_backend.h"
#include "db2/kb_vectors.h"
#include "db2/pgvec_transport.h"
#include "db2/sketch.h"
#include "db2/source_generation.h"
#include "kb_background.h"
#include "kb_curator_queue.h"
#include "log.h"

#include <stdio.h>

#define SOURCE_PRUNE_RETRY_SECONDS 300

typedef int (*source_prune_store_fn)(const char *project);

static int source_prune_external_stores(const db2_source_prune_candidate_t *candidate,
                                        const char *generation, const char *purge_id)
{
   static const struct
   {
      const char *name;
      source_prune_store_fn fn;
   } stores[] = {
       {"chunks", db2_kb_service_clear_project},
       {"file_index", db2_kb_file_index_delete_project},
       {"vectors", pgvec_kb_vector_delete_project},
       {"code_embeddings", pgvec_code_delete_project},
       {"curator_code_unit_vectors", pgvec_curator_code_unit_delete_project},
       {"code_unit_jobs", kb_curator_code_unit_jobs_delete_project},
       {"pdf_vectors", pgvec_kbpdf_delete_project},
       {"minhash", db2_sketch_minhash_signature_delete_project},
   };
   for (size_t i = 0; i < sizeof(stores) / sizeof(stores[0]); i++)
   {
      int rc = stores[i].fn(candidate->physical_project);
      if (rc < 0)
      {
         aimee_log(LOG_WARN, "kb.source_prune",
                   "generation %lld project '%s': store '%s' purge failed",
                   (long long)candidate->generation_id, candidate->physical_project,
                   stores[i].name);
         return -1;
      }
      if (i + 1 < sizeof(stores) / sizeof(stores[0]) &&
          db2_kb_purge_fence_heartbeat(candidate->physical_project, generation, purge_id) != 1)
      {
         aimee_log(LOG_WARN, "kb.source_prune",
                   "generation %lld project '%s': purge fence lost",
                   (long long)candidate->generation_id, candidate->physical_project);
         return -1;
      }
   }
   return 0;
}

int kb_source_prune_sweep(int max_generations)
{
   if (max_generations <= 0)
      return 0;
   int finalized = 0;
   for (int i = 0; i < max_generations; i++)
   {
      db2_source_prune_candidate_t candidate;
      int claimed = db2_source_generation_prune_claim(&candidate);
      if (claimed < 0)
         return finalized > 0 ? finalized : -1;
      if (claimed == 0)
         break;

      char generation[128], purge_id[128];
      snprintf(generation, sizeof(generation), "source-generation:%lld",
               (long long)candidate.generation_id);
      snprintf(purge_id, sizeof(purge_id), "source-gc:%lld",
               (long long)candidate.generation_id);
      kb_background_set("source_prune", "repository=%s generation=%lld",
                        candidate.repository_key, (long long)candidate.generation_id);

      char held_generation[128] = "", held_purge_id[128] = "";
      int replaced = 0;
      int fenced = db2_kb_purge_fence_acquire(
          candidate.physical_project, generation, purge_id, 0, held_generation,
          sizeof(held_generation), held_purge_id, sizeof(held_purge_id), &replaced);
      if (fenced != 1 ||
          source_prune_external_stores(&candidate, generation, purge_id) != 0 ||
          db2_source_generation_prune_finalize(candidate.generation_id) != 1)
      {
         (void)db2_source_generation_prune_release(
             candidate.generation_id,
             fenced == 0 ? "generation prune deferred by active purge fence"
                         : "generation prune store fan-out failed",
             SOURCE_PRUNE_RETRY_SECONDS);
         if (fenced == 1)
            (void)db2_kb_purge_fence_clear(candidate.physical_project, generation, purge_id);
         kb_background_clear("source_prune");
         continue;
      }

      (void)db2_kb_purge_fence_clear(candidate.physical_project, generation, purge_id);
      aimee_log(LOG_INFO, "kb.source_prune",
                "pruned unreferenced generation %lld for repository '%s'",
                (long long)candidate.generation_id, candidate.repository_key);
      finalized++;
      kb_background_clear("source_prune");
   }
   return finalized;
}
