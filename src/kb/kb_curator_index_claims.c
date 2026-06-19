/* kb_curator_index_claims.c: deep-curator claim vector indexer.
 *
 * Claims one proposed `claim` artifact, embeds subject+attribute into the
 * subj_attr named vector and value into the value named vector, upserts the row
 * into curator_claim_vectors, and commits the artifact. The named vectors let
 * detect_contradictions match on subject/attribute while anti-matching on value.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_index_claims.h"
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

/* Buffer cap for the claim subject/value embeddings; actual dimension is set
 * by the deployment's single embedder (1024 pplx-0.6b / 2560 pplx-4b). */
#define CURATOR_CLAIM_DIM EMBED_MAX_DIM

static int64_t claim_point_id(const char *artifact_id)
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

int kb_curator_index_claims_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, payload"
                            " FROM artifacts"
                            " WHERE kind = 'claim' AND state = 'proposed'"
                            " ORDER BY id LIMIT 1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char id[64] = "";
   char *payload = NULL;
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c_id = aimee_pg_column_text(st, 0);
      const char *c_pl = aimee_pg_column_text(st, 1);
      snprintf(id, sizeof(id), "%s", c_id ? c_id : "");
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
   /* subj_attr text is "subject attribute"; fall back to the full claim text so
    * legacy claims (text only) still index. */
   char subj_attr[512] = "";
   const char *subject = pj ? jo_cstr(pj, "subject") : "";
   const char *attribute = pj ? jo_cstr(pj, "attribute") : "";
   const char *value = pj ? jo_cstr(pj, "value") : "";
   const char *claim_kind = pj ? jo_cstr(pj, "claim_kind") : "";
   const char *text = pj ? jo_cstr(pj, "text") : "";
   if (subject[0] || attribute[0])
      snprintf(subj_attr, sizeof(subj_attr), "%s %s", subject, attribute);
   else
      snprintf(subj_attr, sizeof(subj_attr), "%s", text);
   const char *value_text = value[0] ? value : text;

   if (!subj_attr[0] && !value_text[0])
   {
      /* Nothing embeddable — commit so it is not reprocessed. */
      cJSON_Delete(pj);
      db2_artifact_set_state(id, "committed");
      free(payload);
      return 1;
   }

   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = config_embedding_command(&cfg, NULL);
   float subj_vec[CURATOR_CLAIM_DIM];
   float val_vec[CURATOR_CLAIM_DIM];
   int d1 = memory_embed_text(subj_attr[0] ? subj_attr : value_text, embed_cmd, subj_vec,
                              CURATOR_CLAIM_DIM);
   int d2 = memory_embed_text(value_text[0] ? value_text : subj_attr, embed_cmd, val_vec,
                              CURATOR_CLAIM_DIM);
   if (d1 > 0 && d1 == d2)
   {
      int64_t pid = claim_point_id(id);
      if (pgvec_curator_claim_upsert(pid, subj_vec, val_vec, d1, id, subject, attribute, value,
                                     claim_kind, payload ? payload : "{}") != 0)
         aimee_log(LOG_WARN, "kb.curator.claims", "claim vector upsert failed for %s", id);
   }
   else
   {
      aimee_log(LOG_WARN, "kb.curator.claims", "embed failed (d1=%d d2=%d) for claim %s", d1, d2,
                id);
   }

   db2_artifact_set_state(id, "committed");
   aimee_log(LOG_INFO, "kb.curator.claims", "indexed claim %s", id);

   cJSON_Delete(pj);
   free(payload);
   return 1;
}
