/* kb_evidence_embed.c: evidence-vector embed worker.
 *
 * Per pending evidence_index_ops row: read the evidence artifact, extract its
 * "content", embed it via the configured embedding_command (or the builtin
 * hash embedder), format the 384-dim result as a pgvector text literal, and
 * store it through db2_evidence_store_vector (which also marks the op 'ok').
 * On any failure the op is marked failed so the queue makes forward progress.
 * DB2 only; no DB1 access from this file. */

#include "kb_evidence_embed.h"

#include "aimee.h"
#include "cJSON.h"
#include "db2/db2.h" /* db2_lease_release_idle */
#include "db2/artifacts.h"
#include "db2/evidence_vectors.h"
#include "log.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* evidence_vectors.embedding is vector(384); MiniLM and the builtin embedder
 * both emit 384 dims. We require an exact match so a misconfigured sidecar
 * cannot poison the column with the wrong dimensionality. */
#define EVIDENCE_EMBED_DIM 384

/* Pull the embeddable text out of an evidence artifact payload. Evidence rows
 * are written as {"source_kind":...,"content":"..."}; fall back to a couple of
 * alternate keys, then to the whole payload, so we never silently embed "". */
static int evidence_extract_content(const char *payload_json, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   if (!payload_json || !payload_json[0])
      return -1;

   cJSON *root = cJSON_Parse(payload_json);
   if (!root)
   {
      /* Not JSON — embed the raw payload text as a last resort. */
      snprintf(out, out_len, "%s", payload_json);
      return out[0] ? 0 : -1;
   }

   static const char *const keys[] = {"content", "description", "narrative", "value", "title"};
   const char *found = NULL;
   for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
   {
      cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
      if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      {
         found = item->valuestring;
         break;
      }
   }

   if (found)
      snprintf(out, out_len, "%s", found);
   else
      snprintf(out, out_len, "%s", payload_json);

   cJSON_Delete(root);
   return out[0] ? 0 : -1;
}

/* Format a float vector as a pgvector text literal: "[f0,f1,...]".
 * Returns a malloc'd string the caller frees, or NULL on OOM. */
static char *evidence_vec_to_text(const float *vec, int dim)
{
   if (!vec || dim <= 0)
      return NULL;
   /* Each element renders to at most ~16 chars ("-0.123456," etc). */
   size_t cap = (size_t)dim * 18 + 4;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t pos = 0;
   buf[pos++] = '[';
   for (int i = 0; i < dim; i++)
   {
      int n = snprintf(buf + pos, cap - pos, "%s%.6f", i ? "," : "", vec[i]);
      if (n < 0 || (size_t)n >= cap - pos)
      {
         free(buf);
         return NULL;
      }
      pos += (size_t)n;
   }
   if (pos + 2 > cap)
   {
      free(buf);
      return NULL;
   }
   buf[pos++] = ']';
   buf[pos] = '\0';
   return buf;
}

int kb_evidence_embed_one(const char *embed_cmd)
{
   db2_evidence_pending_t pend;
   int got = db2_evidence_list_pending(&pend, 1);
   if (got < 0)
      return -1;
   if (got == 0)
      return 0;

   const char *model = config_embedding_command(NULL, embed_cmd);

   db2_artifact_row_t row;
   if (db2_artifact_read(pend.artifact_id, &row, NULL, 0, NULL) != 0)
   {
      db2_evidence_mark_failed(pend.artifact_id, "artifact not found");
      aimee_log(LOG_WARN, "kb.evidence.embed", "artifact %s missing; op marked failed",
                pend.artifact_id);
      return 1;
   }

   char content[4096];
   if (evidence_extract_content(row.payload_json, content, sizeof(content)) != 0)
   {
      db2_evidence_mark_failed(pend.artifact_id, "no embeddable content");
      return 1;
   }

   /* Drop the pool lease before the embedder round-trip so the evidence-embed
    * drain can't pin a connection past the 300s stuck-lease ceiling (see
    * kb_curator_extract_code / kb_service_code_embed). No-op in a lease scope. */
   db2_lease_release_idle();
   float vec[EVIDENCE_EMBED_DIM];
   int dim = memory_embed_text(content, model, vec, EVIDENCE_EMBED_DIM);
   if (dim != EVIDENCE_EMBED_DIM)
   {
      char err[128];
      snprintf(err, sizeof(err), "embed dim %d != %d", dim, EVIDENCE_EMBED_DIM);
      db2_evidence_mark_failed(pend.artifact_id, err);
      aimee_log(LOG_WARN, "kb.evidence.embed", "%s for %s (model=%s)", err, pend.artifact_id,
                model);
      return 1;
   }

   char *vec_text = evidence_vec_to_text(vec, dim);
   if (!vec_text)
   {
      db2_evidence_mark_failed(pend.artifact_id, "vector format failed");
      return 1;
   }

   int rc = db2_evidence_store_vector(pend.artifact_id, pend.collection, vec_text);
   free(vec_text);
   if (rc != 0)
   {
      db2_evidence_mark_failed(pend.artifact_id, "store failed");
      aimee_log(LOG_WARN, "kb.evidence.embed", "store_vector failed for %s", pend.artifact_id);
      return 1;
   }

   aimee_log(LOG_DEBUG, "kb.evidence.embed", "embedded %s (collection=%s, model=%s)",
             pend.artifact_id, pend.collection, model);
   return 1;
}

int kb_evidence_embed_drain(int max, const char *embed_cmd)
{
   if (max <= 0)
      max = 32;
   int processed = 0;
   for (int i = 0; i < max; i++)
   {
      int rc = kb_evidence_embed_one(embed_cmd);
      if (rc <= 0)
         break; /* 0 = queue drained, -1 = hard DB error */
      processed++;
   }
   return processed;
}
