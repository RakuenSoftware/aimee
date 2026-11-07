#include "module_commands.h"
#include "json_fluent.h"
#include "embed_result.h"
/* kb_evidence_embed.c: evidence-vector embed worker.
 *
 * Per pending evidence_index_ops row: read the evidence artifact, extract its
 * "content", embed it via the configured embedding_command (or the builtin
 * hash embedder), format the configured-width result as a pgvector text literal, and
 * store it through kb_store_evidence_store_vector (which also marks the op 'ok').
 * On any failure the op is marked failed so the queue makes forward progress.
 * KB_STORE only; no DB1 access from this file. */

#include "kb_evidence_embed.h"

#include "aimee.h"
#include "config_database.h"
#include "cJSON.h"
#include "modules/kb/c/kb_store.h" /* kb_store_lease_release_idle */
#include "modules/kb/c/artifacts.h"
#include "modules/kb/c/evidence_vectors.h"
#include "log.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Evidence vectors use the instance's configured width. Require an exact
 * response length so a misconfigured embedder cannot poison the column. */

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
   kb_store_evidence_pending_t pend;
   int got = kb_store_evidence_list_pending(&pend, 1);
   if (got < 0)
      return -1;
   if (got == 0)
      return 0;

   const char *model = config_embedder_command_current(embed_cmd);

   kb_store_artifact_row_t row;
   if (kb_store_artifact_read(pend.artifact_id, &row, NULL, 0, NULL) != 0)
   {
      kb_store_evidence_mark_failed(pend.artifact_id, "artifact not found");
      aimee_log(LOG_WARN, "kb.evidence.embed", "artifact %s missing; op marked failed",
                pend.artifact_id);
      return 1;
   }

   char content[4096];
   if (evidence_extract_content(row.payload_json, content, sizeof(content)) != 0)
   {
      kb_store_evidence_mark_failed(pend.artifact_id, "no embeddable content");
      return 1;
   }

   /* Drop the pool lease before the embedder round-trip so the evidence-embed
    * drain can't pin a connection past the 300s stuck-lease ceiling (see
    * kb_curator_extract_code / kb_service_code_embed). No-op in a lease scope. */
   kb_store_lease_release_idle();
   const int embed_dim = config_embedder_dims_current();
   if (embed_dim <= 0 || embed_dim > EMBED_MAX_DIM)
   {
      kb_store_evidence_mark_failed(pend.artifact_id, "invalid configured embedding dimension");
      return 1;
   }
   float *vec = malloc((size_t)embed_dim * sizeof(*vec));
   if (!vec)
   {
      kb_store_evidence_mark_failed(pend.artifact_id, "vector allocation failed");
      return 1;
   }
   cJSON *embed_0_args = cJSON_CreateObject(), *embed_0_reply = NULL;
   cJSON_AddStringToObject(embed_0_args, "base_url", model);
   cJSON_AddStringToObject(embed_0_args, "input_type", "document");
   cJSON_AddStringToObject(embed_0_args, "text", content);
   cJSON_AddNumberToObject(embed_0_args, "max_dim", embed_dim);
   (void)aimee_module_commands_dispatch_internal("memory.embed_text", embed_0_args, &embed_0_reply);
   cJSON_Delete(embed_0_args);
   int dim = embed_reply_vector(embed_0_reply, vec, embed_dim);
   cJSON_Delete(embed_0_reply);
   if (dim != embed_dim)
   {
      free(vec);
      char err[128];
      snprintf(err, sizeof(err), "embed dim %d != %d", dim, embed_dim);
      kb_store_evidence_mark_failed(pend.artifact_id, err);
      aimee_log(LOG_WARN, "kb.evidence.embed", "%s for %s (model=%s)", err, pend.artifact_id,
                model);
      return 1;
   }

   char *vec_text = evidence_vec_to_text(vec, dim);
   free(vec);
   if (!vec_text)
   {
      kb_store_evidence_mark_failed(pend.artifact_id, "vector format failed");
      return 1;
   }

   int rc = kb_store_evidence_store_vector(pend.artifact_id, pend.collection, vec_text);
   free(vec_text);
   if (rc != 0)
   {
      kb_store_evidence_mark_failed(pend.artifact_id, "store failed");
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
