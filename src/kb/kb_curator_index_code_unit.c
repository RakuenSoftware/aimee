/* kb_curator_index_code_unit.c: deep-curator code_unit vector indexer.
 *
 * Claims one proposed `code_unit` artifact, embeds its intent (summary),
 * signature, and body excerpt into the three named vectors, upserts the row into
 * curator_code_unit_vectors, and commits the artifact. The named vectors let a
 * query rank on whichever facet it targets (what the code is for, its shape, or
 * its implementation). No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_index_code_unit.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer cap for the intent/signature/body embeddings; actual dimension is set
 * by the deployment's single embedder (1024 pplx-0.6b / 2560 pplx-4b). */
#define CURATOR_CODE_UNIT_DIM EMBED_MAX_DIM

static int64_t fnv1a(const char *s)
{
   uint64_t h = 1469598103934665603ULL;
   if (s)
   {
      for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      {
         h ^= (uint64_t)*p;
         h *= 1099511628211ULL;
      }
   }
   return (int64_t)(h & 0x7FFFFFFFFFFFFFFFULL);
}

int kb_curator_index_code_unit_one(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id, payload"
                            " FROM artifacts"
                            " WHERE kind = 'code_unit' AND state = 'proposed'"
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
   const char *intent = pj ? jo_cstr(pj, "summary") : "";
   const char *signature = pj ? jo_cstr(pj, "signature") : "";
   const char *body = pj ? jo_cstr(pj, "body_excerpt") : "";
   const char *file_path = pj ? jo_cstr(pj, "file_path") : "";
   const char *def_kind = pj ? jo_cstr(pj, "def_kind") : "";

   /* Each named vector falls back to the next-best available text so a sparse
    * payload still produces three valid vectors. */
   const char *intent_text = intent[0] ? intent : (signature[0] ? signature : body);
   const char *sig_text = signature[0] ? signature : (intent[0] ? intent : body);
   const char *body_text = body[0] ? body : (signature[0] ? signature : intent);

   if (!intent_text[0] && !sig_text[0] && !body_text[0])
   {
      cJSON_Delete(pj);
      db2_artifact_set_state(id, "committed");
      free(payload);
      return 1;
   }

   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = config_embedding_command(&cfg, NULL);
   float intent_vec[CURATOR_CODE_UNIT_DIM];
   float sig_vec[CURATOR_CODE_UNIT_DIM];
   float body_vec[CURATOR_CODE_UNIT_DIM];
   int d1 = memory_embed_text(intent_text, embed_cmd, intent_vec, CURATOR_CODE_UNIT_DIM);
   int d2 = memory_embed_text(sig_text, embed_cmd, sig_vec, CURATOR_CODE_UNIT_DIM);
   int d3 = memory_embed_text(body_text, embed_cmd, body_vec, CURATOR_CODE_UNIT_DIM);
   if (d1 > 0 && d1 == d2 && d2 == d3)
   {
      int64_t pid = fnv1a(id);
      char body_hash[32];
      snprintf(body_hash, sizeof(body_hash), "%llx", (unsigned long long)fnv1a(body));
      if (pgvec_curator_code_unit_upsert(pid, intent_vec, sig_vec, body_vec, d1, id, file_path,
                                         def_kind, signature, body_hash,
                                         payload ? payload : "{}") != 0)
         aimee_log(LOG_WARN, "kb.curator.code_unit", "code_unit vector upsert failed for %s", id);
   }
   else
   {
      aimee_log(LOG_WARN, "kb.curator.code_unit", "embed failed (d=%d/%d/%d) for code_unit %s", d1,
                d2, d3, id);
   }

   db2_artifact_set_state(id, "committed");
   aimee_log(LOG_INFO, "kb.curator.code_unit", "indexed code_unit %s", id);

   cJSON_Delete(pj);
   free(payload);
   return 1;
}
