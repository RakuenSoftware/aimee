/* kb_curator_index_narrative.c: deep-curator narrative vector indexer.
 *
 * Claims one proposed doc_summary / synthesis / open_question artifact, embeds
 * its narrative text via the configured embedding_command, upserts it into
 * curator_narrative_vectors with its typed facets (kind/doc_id/status/priority),
 * and commits the artifact. No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_index_narrative.h"
#include "json_fluent.h"
#include "aimee.h"
#include "config.h"
#include "cJSON.h"
#include "log.h"
#include "memory.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/pgvec_transport.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CURATOR_NARRATIVE_DIM 384

/* Deterministic, positive 64-bit point id (FNV-1a) for a narrative artifact. */
static int64_t narrative_point_id(const char *artifact_id)
{
   uint64_t h = 1469598103934665603ULL;
   if (artifact_id)
   {
      for (const unsigned char *p = (const unsigned char *)artifact_id; *p; p++)
      {
         h ^= (uint64_t)*p;
         h *= 1099511628211ULL;
      }
   }
   return (int64_t)(h & 0x7FFFFFFFFFFFFFFFULL);
}

/* Pull the embeddable narrative text out of a payload: prefer summary, then
 * text, then body. Returns a pointer into the cJSON tree (valid while pj is). */
static const char *narrative_text(const cJSON *pj)
{
   const char *keys[] = {"summary", "text", "body", NULL};
   for (int i = 0; keys[i]; i++)
   {
      const cJSON *j = cJSON_GetObjectItemCaseSensitive(pj, keys[i]);
      if (cJSON_IsString(j) && j->valuestring[0])
         return j->valuestring;
   }
   return NULL;
}

int kb_curator_index_narrative_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "SELECT id, kind, payload, scope_kind, scope_id"
       " FROM artifacts"
       " WHERE kind IN ('doc_summary', 'synthesis', 'open_question') AND state = 'proposed'"
       " ORDER BY id LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char id[64] = "", kind[64] = "";
   char *payload = NULL;
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_kind = aimee_pg_column_text(st, 1);
      const char *c_pl = aimee_pg_column_text(st, 2);
      snprintf(id, sizeof(id), "%s", c_id ? c_id : "");
      snprintf(kind, sizeof(kind), "%s", c_kind ? c_kind : "");
      payload = strdup(c_pl ? c_pl : "{}");
      found = 1;
   }
   aimee_pg_finalize(st);
   if (!found)
   {
      free(payload);
      return 0;
   }

   cJSON *pj = payload ? cJSON_Parse(payload) : NULL;
   const char *text = pj ? narrative_text(pj) : NULL;
   if (!text)
   {
      /* Nothing embeddable — commit so it is not reprocessed. */
      cJSON_Delete(pj);
      db2_artifact_set_state(id, "committed");
      free(payload);
      return 1;
   }

   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";
   float vec[CURATOR_NARRATIVE_DIM];
   int dim = memory_embed_text(text, embed_cmd, vec, CURATOR_NARRATIVE_DIM);
   if (dim == CURATOR_NARRATIVE_DIM)
   {
      int64_t pid = narrative_point_id(id);
      const char *doc_id = jo_cstr(pj, "doc_id");
      const char *status = jo_cstr(pj, "status");
      const char *priority = jo_cstr(pj, "priority");
      if (pgvec_curator_narrative_upsert(pid, vec, dim, id, kind, doc_id, status, priority,
                                         payload ? payload : "{}") != 0)
         aimee_log(LOG_WARN, "kb.curator.narrative", "narrative vector upsert failed for %s", id);
      else if (cfg.memory_deep_embedding_enabled && cfg.memory_deep_embedding_command[0])
      {
/* Deep tier: store a deep embedding, then link deep-confirmed near-duplicate
 * narratives via an additive "similar_to" edge (never drops content). Conservative
 * thresholds; gated default-off. */
#define CURATOR_DEDUP_K        5
#define CURATOR_DEDUP_LIVE_MIN 0.85
#define CURATOR_DEDUP_DEEP_MIN 0.85
         float deep_vec[DEEP_EMBED_MAX_DIM];
         int dd = memory_embed_text(text, cfg.memory_deep_embedding_command, deep_vec,
                                    DEEP_EMBED_MAX_DIM);
         if (dd > 0)
         {
            int64_t cand[CURATOR_DEDUP_K];
            double cscore[CURATOR_DEDUP_K];
            int nc = pgvec_curator_narrative_search(NULL, NULL, NULL, vec, dim, CURATOR_DEDUP_K,
                                                    cand, cscore, CURATOR_DEDUP_K);
            for (int j = 0; j < nc; j++)
            {
               if (cand[j] == pid || cscore[j] < CURATOR_DEDUP_LIVE_MIN)
                  continue;
               double dsim = 0.0;
               if (pgvec_curator_narrative_deep_similarity(cand[j], deep_vec, dd, &dsim) != 1 ||
                   dsim < CURATOR_DEDUP_DEEP_MIN)
                  continue;
               char cart[64] = "";
               if (pgvec_curator_narrative_artifact(cand[j], cart, sizeof(cart)) == 1 && cart[0] &&
                   strcmp(cart, id) != 0)
                  db2_artifact_link(id, cart, "similar_to");
            }
            pgvec_curator_narrative_deep_update(pid, deep_vec, dd);
         }
      }
   }
   else
   {
      aimee_log(LOG_WARN, "kb.curator.narrative", "embed failed (dim=%d) for narrative %s", dim,
                id);
   }

   db2_artifact_set_state(id, "committed");
   aimee_log(LOG_INFO, "kb.curator.narrative", "indexed %s artifact %s", kind, id);

   cJSON_Delete(pj);
   free(payload);
   return 1;
}
