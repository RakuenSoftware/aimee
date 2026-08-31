/* kb_learning_synth.c: cross-source learning candidate-generation pass.
 *
 * bundle (learning_bundle) -> sidecar (model) -> proposed candidate artifacts,
 * each citing the neighbourhood evidence so judge/promote can act unchanged.
 * DB2 only; no DB1 access from this file. */

#include "kb_learning_synth.h"

#include "aimee.h"
#include "cJSON.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/learning_synth_ops.h"
#include "modules/learning/learning_bundle.h"
#include "log.h"
#include "platform_process.h" /* platform_exec_pipe */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull embeddable/summarisable text from an evidence artifact payload, falling
 * back through alternate keys then the whole payload (mirrors the embed worker
 * so the sidecar sees the same text the vector was built from). */
static void synth_extract_content(const char *payload_json, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!payload_json || !payload_json[0])
      return;

   cJSON *root = cJSON_Parse(payload_json);
   if (!root)
   {
      snprintf(out, out_len, "%s", payload_json);
      return;
   }
   static const char *const keys[] = {"content", "description", "narrative", "value", "title"};
   const char *found = NULL;
   for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
   {
      cJSON *it = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
      if (it && cJSON_IsString(it) && it->valuestring && it->valuestring[0])
      {
         found = it->valuestring;
         break;
      }
   }
   snprintf(out, out_len, "%s", found ? found : payload_json);
   cJSON_Delete(root);
}

/* Serialise the synthesis request (role + neighbourhood + config) to JSON. */
static char *synth_build_request(const char *query, const learning_bundle_t *bundle, int max_tokens)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON_AddStringToObject(root, "role", "synthesize");

   cJSON *input = cJSON_AddObjectToObject(root, "input");
   cJSON_AddStringToObject(input, "query", query);
   cJSON *neighbours = cJSON_AddArrayToObject(input, "neighbours");

   for (int i = 0; i < bundle->count; i++)
   {
      const learning_bundle_item_t *bi = &bundle->items[i];
      db2_artifact_row_t row;
      char content[4096] = "";
      if (db2_artifact_read(bi->artifact_id, &row, NULL, 0, NULL) == 0)
         synth_extract_content(row.payload_json, content, sizeof(content));

      cJSON *n = cJSON_CreateObject();
      cJSON_AddStringToObject(n, "artifact_id", bi->artifact_id);
      cJSON_AddStringToObject(n, "kind", bi->kind);
      cJSON_AddStringToObject(n, "content", content);
      cJSON_AddNumberToObject(n, "score", bi->score);
      cJSON_AddItemToArray(neighbours, n);
   }

   cJSON *config = cJSON_AddObjectToObject(root, "config");
   cJSON_AddNumberToObject(config, "max_tokens", max_tokens > 0 ? max_tokens : 2048);

   char *str = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return str;
}

int kb_learning_synth_generate(const char *query, const char *synth_cmd, const char *embed_cmd,
                               int k, int max_tokens, const char *scope_kind, const char *scope_id,
                               const char *operator_id, char proposed_ids[][37], int max_ids)
{
   if (!query || !query[0] || !synth_cmd || !synth_cmd[0])
      return -1;

   learning_bundle_t bundle;
   if (learning_bundle_build(query, embed_cmd, k, &bundle) != 0)
      return -1;
   if (bundle.count == 0)
      return 0; /* nothing to synthesise from */

   char *req = synth_build_request(query, &bundle, max_tokens);
   if (!req)
      return -1;

   char *resp = NULL;
   size_t resp_len = 0;
   int rc = platform_exec_pipe(synth_cmd, req, strlen(req), &resp, &resp_len);
   free(req);
   if (rc != 0 || !resp)
   {
      aimee_log(LOG_WARN, "kb.learning.synth", "sidecar failed (exit %d)", rc);
      free(resp);
      return -1;
   }

   cJSON *root = cJSON_Parse(resp);
   free(resp);
   if (!root)
   {
      aimee_log(LOG_WARN, "kb.learning.synth", "sidecar returned invalid JSON");
      return -1;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   if (status && cJSON_IsString(status) && status->valuestring &&
       strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *errj = cJSON_GetObjectItemCaseSensitive(root, "error");
      aimee_log(LOG_WARN, "kb.learning.synth", "sidecar error: %s",
                (errj && cJSON_IsString(errj)) ? errj->valuestring : "(unspecified)");
      cJSON_Delete(root);
      return -1;
   }

   cJSON *candidates = cJSON_GetObjectItemCaseSensitive(root, "candidates");
   if (!candidates || !cJSON_IsArray(candidates))
   {
      cJSON_Delete(root);
      return 0;
   }

   int written = 0;
   cJSON *c;
   cJSON_ArrayForEach(c, candidates)
   {
      if (!cJSON_IsObject(c))
         continue;
      cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(c, "kind");
      cJSON *payload_j = cJSON_GetObjectItemCaseSensitive(c, "payload");
      if (!kind_j || !cJSON_IsString(kind_j) || !kind_j->valuestring || !kind_j->valuestring[0])
         continue;
      const char *kind = kind_j->valuestring;

      /* Honour the substrate's rejection suppression: don't re-propose a
       * candidate kind that was thumbs-downed for this scope. */
      if (db2_artifact_verdict_suppressed(kind, scope_id ? scope_id : ""))
      {
         aimee_log(LOG_DEBUG, "kb.learning.synth", "suppressed candidate kind=%s scope=%s", kind,
                   scope_id ? scope_id : "");
         continue;
      }

      double confidence = 0.0;
      cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(c, "confidence");
      if (conf_j && cJSON_IsNumber(conf_j))
         confidence = conf_j->valuedouble;

      char *payload_str = NULL;
      if (payload_j)
         payload_str = cJSON_PrintUnformatted(payload_j);

      char id[64];
      db2_artifact_gen_id(id, sizeof(id));
      int wrc = db2_artifact_write(id, kind, "proposed", scope_kind, scope_id, operator_id,
                                   confidence, payload_str ? payload_str : "{}");
      free(payload_str);
      if (wrc != 0)
         continue;

      /* Cite every neighbourhood evidence artifact as a corroborating source so
       * the judge's distinct-source count reflects the neighbourhood. */
      for (int i = 0; i < bundle.count; i++)
         db2_artifact_cite(id, bundle.items[i].kind, bundle.items[i].artifact_id);

      if (proposed_ids && written < max_ids)
         snprintf(proposed_ids[written], 37, "%s", id);
      written++;

      aimee_log(LOG_DEBUG, "kb.learning.synth", "proposed candidate %s kind=%s (cited %d sources)",
                id, kind, bundle.count);
   }

   cJSON_Delete(root);
   return written;
}

int kb_learning_synth_drain(int max, const char *synth_cmd, const char *embed_cmd, int k,
                            int max_tokens)
{
   if (!synth_cmd || !synth_cmd[0])
      return 0;
   if (max <= 0)
      max = 16;

   char pend[64][37];
   int cap = max < 64 ? max : 64;
   int got = db2_synth_list_pending(pend, cap);
   if (got <= 0)
      return got < 0 ? -1 : 0;

   int processed = 0;
   for (int i = 0; i < got; i++)
   {
      db2_artifact_row_t row;
      if (db2_artifact_read(pend[i], &row, NULL, 0, NULL) != 0)
      {
         /* Evidence vanished — retire the op so the queue advances. */
         db2_synth_mark_done(pend[i]);
         processed++;
         continue;
      }

      char query[4096];
      synth_extract_content(row.payload_json, query, sizeof(query));

      int n = kb_learning_synth_generate(query[0] ? query : "synthesize", synth_cmd, embed_cmd, k,
                                         max_tokens, row.scope_kind, row.scope_id, "synthesis",
                                         NULL, 0);
      if (n < 0)
         db2_synth_mark_failed(pend[i], "synthesis failed");
      else
         db2_synth_mark_done(pend[i]);
      processed++;
   }
   return processed;
}
