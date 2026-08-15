#include "pgvec_kb_service.h"
#include "memory_vectors.h"
#include "pgvec_transport.h"

#include <stdlib.h>
#include <string.h>

int pgvec_kb_service_ensure_kb_collection(int dim)
{
   return pgvec_ensure_index(PGVEC_KB_TABLE, dim, 0);
}

int pgvec_kb_service_ensure_memory_collection(int dim)
{
   return pgvec_ensure_index(PGVEC_MEMORY_TABLE, dim, 0);
}

int pgvec_kb_service_reconcile_orphans(pgvec_kb_service_record_exists_fn mem_exists,
                                       pgvec_kb_service_record_exists_fn kb_exists, int dry_run,
                                       pgvec_kb_service_reconcile_result_t *out)
{
   if (!out || !mem_exists || !kb_exists)
      return -1;

   memset(out, 0, sizeof(*out));

   const int batch = 256;
   int64_t ids[256];

   /* Scan memory_embeddings and prune orphans. */
   int64_t offset = -1;
   int done = 0;
   while (!done)
   {
      int n = pgvec_scroll(PGVEC_MEMORY_TABLE, offset, ids, batch, &offset, &done);
      if (n < 0)
      {
         out->rc = -1;
         return 0;
      }
      for (int i = 0; i < n; i++)
      {
         int exists = mem_exists(ids[i]);
         if (exists > 0)
         {
            out->mem_kept++;
         }
         else if (exists == 0)
         {
            out->mem_pruned++;
            if (!dry_run)
               pgvec_memory_delete(ids[i]);
         }
         else
         {
            out->rc = -1;
            return 0;
         }
      }
   }

   /* Scan kb_embeddings and prune orphans. */
   offset = -1;
   done = 0;
   while (!done)
   {
      int n = pgvec_scroll(PGVEC_KB_TABLE, offset, ids, batch, &offset, &done);
      if (n < 0)
      {
         out->rc = -1;
         return 0;
      }
      for (int i = 0; i < n; i++)
      {
         int exists = kb_exists(ids[i]);
         if (exists > 0)
         {
            out->kb_kept++;
         }
         else if (exists == 0)
         {
            out->kb_pruned++;
            if (!dry_run)
               pgvec_kb_delete(ids[i]);
         }
         else
         {
            out->rc = -1;
            return 0;
         }
      }
   }

   out->rc = 0;
   return 0;
}

int pgvec_kb_service_search_memory_points(const char *record_type, const float *vec, int dim,
                                          int limit, int64_t *ids, double *scores, int max)
{
   return pgvec_memory_vector_search_record_type(record_type, vec, dim, limit, ids, scores, max);
}

int pgvec_kb_service_upsert_document_point(int64_t doc_id, const float *vec, int dim,
                                           const char *payload_json)
{
   return pgvec_kb_upsert(doc_id, vec, dim, payload_json);
}

/* --- Phase 5: code embedding helpers --- */

int pgvec_kb_service_ensure_code_collection(int dim)
{
   return pgvec_ensure_index(PGVEC_CODE_TABLE, dim, 0);
}

int pgvec_kb_service_code_upsert(int64_t point_id, const float *vec, int dim, const char *project,
                                 const char *node_key, const char *file_path, const char *symbol,
                                 const char *content_hash, const char *body_hash,
                                 const char *source_hash, const char *payload_json)
{
   return pgvec_code_upsert(point_id, vec, dim, project, node_key, file_path, symbol, content_hash,
                            body_hash, source_hash, payload_json);
}

int pgvec_kb_service_code_exists_by_hash(const char *project, const char *node_key,
                                         const char *content_hash, const char *body_hash)
{
   return pgvec_code_exists_by_hash(project, node_key, content_hash, body_hash);
}
