/* db2/kb_service_backend_context.c: temporal semantic recall and default-on
 * typed context assembly. Kept separate from the compatibility memory RPCs so
 * the context contract can evolve without growing their translation unit. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "config.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "db2_learning.h"
#include "lifecycle.h"
#include "memory.h"
#include "memory_scope_query.h"
#include "memory_vectors.h"
#include "pgvec_transport.h"
#include "typed_facts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int kbs_semantic_assertion_index_refresh(int max_rows)
{
   int indexed = 0;
   semantic_assertion_index_row_t rows[32];
   while (indexed < max_rows)
   {
      int want = max_rows - indexed;
      if (want > 32)
         want = 32;
      int n = db2_semantic_assertion_index_list(0, rows, want);
      if (n <= 0)
         return n < 0 && indexed == 0 ? -1 : indexed;
      for (int i = 0; i < n; i++)
      {
         float vec[EMBED_MAX_DIM];
         const char *embed_cmd = config_embedder_command_current(NULL);
         int dim = memory_embed_text(rows[i].canonical_rendering, embed_cmd, EMBED_INPUT_DOCUMENT,
                                     vec, EMBED_MAX_DIM);
         if (dim <= 0 || dim != db2_embedding_dim())
            return indexed > 0 ? indexed : -1;
         cJSON *payload = cJSON_CreateObject();
         if (!payload)
            return indexed > 0 ? indexed : -1;
         char kind[64];
         snprintf(kind, sizeof(kind), "assertion_v%d", rows[i].version);
         cJSON_AddStringToObject(payload, "record_type", "semantic_assertion");
         cJSON_AddStringToObject(payload, "kind", kind);
         cJSON_AddNumberToObject(payload, "assertion_id", (double)rows[i].assertion_id);
         cJSON_AddNumberToObject(payload, "assertion_version", rows[i].version);
         cJSON_AddStringToObject(payload, "canonical_rendering", rows[i].canonical_rendering);
         cJSON_AddStringToObject(payload, "model_version", config_embedder_model());
         char *payload_json = cJSON_PrintUnformatted(payload);
         cJSON_Delete(payload);
         if (!payload_json)
            return indexed > 0 ? indexed : -1;
         int rc = pgvec_memory_upsert(SEMANTIC_ASSERTION_VECTOR_POINT_OFFSET + rows[i].assertion_id,
                                      vec, dim, payload_json);
         free(payload_json);
         if (rc != 0)
            return indexed > 0 ? indexed : -1;
         indexed++;
      }
      if (n < want)
         break;
   }
   return indexed;
}

static int kbs_semantic_hit_index(semantic_assertion_hit_t *hits, int n, int64_t assertion_id)
{
   for (int i = 0; i < n; i++)
      if (hits[i].assertion_id == assertion_id)
         return i;
   return -1;
}

static void kbs_semantic_add_trace(semantic_assertion_hit_t *hit, const char *channel, double raw,
                                   int rank)
{
   if (!hit || hit->retrieval_count >= SEMANTIC_ASSERTION_TRACE_MAX)
      return;
   semantic_assertion_retrieval_trace_t *trace = &hit->retrieval[hit->retrieval_count++];
   snprintf(trace->channel, sizeof(trace->channel), "%s", channel);
   trace->raw_score = raw;
   trace->rank = rank;
}

static int kbs_semantic_hit_cmp(const void *a, const void *b)
{
   const semantic_assertion_hit_t *ha = a;
   const semantic_assertion_hit_t *hb = b;
   if (ha->fused_score != hb->fused_score)
      return ha->fused_score < hb->fused_score ? 1 : -1;
   if (ha->authority_rank != hb->authority_rank)
      return hb->authority_rank - ha->authority_rank;
   if (ha->assertion_id == hb->assertion_id)
      return 0;
   return ha->assertion_id < hb->assertion_id ? 1 : -1;
}

static int kbs_semantic_assertion_hybrid(const char *query, const char *valid_at,
                                         const char *believed_at, int include_historical,
                                         int max_hops, semantic_assertion_hit_t *hits, int max,
                                         int *vector_available, int *indexed_out,
                                         int *lexical_only_out, int *vector_only_out,
                                         int *overlap_out)
{
   int gather = max * 4;
   if (gather > 64)
      gather = 64;
   int n = db2_semantic_assertion_search(query, valid_at, believed_at, include_historical, gather,
                                         hits, max);
   if (n < 0)
      return n;
   int lexical_n = n;
   *vector_available = 0;
   *indexed_out = kbs_semantic_assertion_index_refresh(256);

   float qvec[EMBED_MAX_DIM];
   const char *embed_cmd = config_embedder_command_current(NULL);
   int qdim = memory_embed_text(query, embed_cmd, EMBED_INPUT_QUERY, qvec, EMBED_MAX_DIM);
   int64_t vector_ids[64];
   double vector_scores[64];
   int vector_n = 0;
   if (qdim > 0 && qdim == db2_embedding_dim())
   {
      vector_n = pgvec_memory_vector_search_record_type("semantic_assertion", qvec, qdim, gather,
                                                        vector_ids, vector_scores, 64);
      if (vector_n >= 0)
         *vector_available = 1;
   }
   if (vector_n < 0)
      vector_n = 0;

   int overlap = 0;
   for (int i = 0; i < vector_n; i++)
   {
      /* Top-k always returns the nearest rows, even when every row is
       * unrelated. Do not let tail neighbors become depth-zero evidence or
       * short-circuit graph-hop provenance. Lexical recall remains available
       * when no vector candidate clears this conservative cosine floor. */
      if (vector_scores[i] < 0.20)
         continue;
      int64_t assertion_id = vector_ids[i] - SEMANTIC_ASSERTION_VECTOR_POINT_OFFSET;
      if (assertion_id <= 0)
         continue;
      int at = kbs_semantic_hit_index(hits, n, assertion_id);
      if (at < 0)
      {
         if (n >= max || db2_semantic_assertion_get_filtered(assertion_id, valid_at, believed_at,
                                                             include_historical, &hits[n]) != 1)
            continue;
         at = n++;
         snprintf(hits[at].inclusion_reason, sizeof(hits[at].inclusion_reason),
                  "vector semantic match after lifecycle, authority, scope, and temporal filters");
      }
      else
         overlap++;
      kbs_semantic_add_trace(&hits[at], "vector", vector_scores[i], i + 1);
   }

   /* Bounded graph expansion is deliberately late and reuses the filtered
    * semantic query for every hop, so no historical or unauthorized edge can
    * re-enter through traversal. */
   int frontier_start = 0;
   int frontier_end = n;
   for (int hop = 1; hop <= max_hops && frontier_start < frontier_end; hop++)
   {
      int next_end = n;
      for (int i = frontier_start; i < frontier_end && n < max; i++)
      {
         const char *anchors[2] = {hits[i].subject, hits[i].object};
         for (int a = 0; a < 2 && n < max; a++)
         {
            semantic_assertion_hit_t expanded[16];
            int en = db2_semantic_assertion_search(anchors[a], valid_at, believed_at,
                                                   include_historical, 16, expanded, 16);
            for (int e = 0; e < en && n < max; e++)
            {
               if (expanded[e].assertion_id == hits[i].assertion_id ||
                   (strcmp(expanded[e].subject, anchors[a]) != 0 &&
                    strcmp(expanded[e].object, anchors[a]) != 0) ||
                   kbs_semantic_hit_index(hits, n, expanded[e].assertion_id) >= 0)
                  continue;
               hits[n] = expanded[e];
               hits[n].hop_depth = hop;
               kbs_semantic_add_trace(&hits[n], "semantic_graph", 1.0 / (double)(hop + 1), n + 1);
               snprintf(hits[n].inclusion_reason, sizeof(hits[n].inclusion_reason),
                        "bounded semantic hop %d with temporal and scope filters reapplied", hop);
               n++;
            }
         }
      }
      frontier_start = frontier_end;
      frontier_end = n;
      if (frontier_end == next_end)
         break;
   }

   int lexical_only = 0;
   int vector_only = 0;
   for (int i = 0; i < n; i++)
   {
      double fused = 0.0;
      int has_lexical = 0;
      int has_vector = 0;
      for (int t = 0; t < hits[i].retrieval_count; t++)
      {
         semantic_assertion_retrieval_trace_t *trace = &hits[i].retrieval[t];
         fused += 1.0 / (60.0 + (double)(trace->rank > 0 ? trace->rank : 1));
         has_lexical |= strcmp(trace->channel, "lexical") == 0;
         has_vector |= strcmp(trace->channel, "vector") == 0;
      }
      hits[i].fused_score = fused;
      if (has_lexical && !has_vector)
         lexical_only++;
      if (has_vector && !has_lexical)
         vector_only++;
   }
   qsort(hits, (size_t)n, sizeof(hits[0]), kbs_semantic_hit_cmp);
   for (int i = 0; i < n; i++)
   {
      hits[i].rank = i + 1;
      for (int t = 0; t < hits[i].retrieval_count; t++)
         hits[i].retrieval[t].fused_score = hits[i].fused_score;
   }
   *lexical_only_out = lexical_only;
   *vector_only_out = vector_only;
   *overlap_out = overlap > lexical_n ? lexical_n : overlap;
   return n > max ? max : n;
}

static cJSON *kbs_semantic_assertion_to_json(const semantic_assertion_hit_t *hit)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "assertion_id", (double)hit->assertion_id);
   cJSON_AddNumberToObject(obj, "version", hit->version);
   cJSON_AddStringToObject(obj, "subject", hit->subject);
   cJSON_AddStringToObject(obj, "relation", hit->relation);
   cJSON_AddStringToObject(obj, "object", hit->object);
   cJSON_AddStringToObject(obj, "assertion_kind", hit->assertion_kind);
   cJSON_AddStringToObject(obj, "lifecycle_state", hit->lifecycle_state);
   cJSON_AddNumberToObject(obj, "authority_rank", hit->authority_rank);
   cJSON_AddStringToObject(obj, "confidence_class", hit->confidence_class);
   cJSON_AddNumberToObject(obj, "confidence", hit->confidence);
   cJSON_AddStringToObject(obj, "valid_from", hit->valid_from);
   cJSON_AddStringToObject(obj, "valid_until", hit->valid_until);
   cJSON_AddStringToObject(obj, "asserted_at", hit->asserted_at);
   cJSON_AddStringToObject(obj, "superseded_at", hit->superseded_at);
   cJSON_AddBoolToObject(obj, "historical", hit->historical);
   cJSON_AddNumberToObject(obj, "support_count", hit->support_count);
   cJSON_AddNumberToObject(obj, "contradiction_count", hit->contradiction_count);
   cJSON *evidence = cJSON_AddArrayToObject(obj, "evidence");
   cJSON *retrieval = cJSON_AddArrayToObject(obj, "retrieval");
   if (!evidence || !retrieval)
   {
      cJSON_Delete(obj);
      return NULL;
   }
   for (int i = 0; i < hit->evidence_count; i++)
   {
      const semantic_assertion_evidence_t *ev = &hit->evidence[i];
      cJSON *item = cJSON_CreateObject();
      if (!item)
         continue;
      cJSON_AddStringToObject(item, "source_kind", ev->source_kind);
      cJSON_AddStringToObject(item, "source_id", ev->source_id);
      cJSON_AddStringToObject(item, "source_span", ev->source_span);
      cJSON_AddStringToObject(item, "observed_at", ev->observed_at);
      cJSON_AddStringToObject(item, "stance", ev->stance);
      cJSON_AddItemToArray(evidence, item);
   }
   for (int i = 0; i < hit->retrieval_count; i++)
   {
      cJSON *trace = cJSON_CreateObject();
      if (!trace)
         continue;
      cJSON_AddStringToObject(trace, "channel", hit->retrieval[i].channel);
      cJSON_AddNumberToObject(trace, "raw_score", hit->retrieval[i].raw_score);
      cJSON_AddNumberToObject(trace, "fused_score", hit->retrieval[i].fused_score);
      cJSON_AddNumberToObject(trace, "rank", hit->retrieval[i].rank);
      cJSON_AddItemToArray(retrieval, trace);
   }
   cJSON_AddNumberToObject(obj, "hop_depth", hit->hop_depth);
   cJSON_AddStringToObject(obj, "inclusion_reason", hit->inclusion_reason);
   return obj;
}

cJSON *db2_kb_service_memory_search_assertions_json(const char *query, const char *valid_at,
                                                    const char *believed_at, int include_historical,
                                                    int max_hops, int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "assertions") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   semantic_assertion_hit_t hits[64];
   int vector_available = 0, indexed = 0, lexical_only = 0, vector_only = 0, overlap = 0;
   int n = kbs_semantic_assertion_hybrid(query ? query : "", valid_at ? valid_at : "",
                                         believed_at ? believed_at : "", include_historical,
                                         max_hops, hits, limit, &vector_available, &indexed,
                                         &lexical_only, &vector_only, &overlap);
   if (n == SEMANTIC_ASSERTION_SEARCH_INVALID_TIME)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "error_type", "invalid_timestamp");
      cJSON_AddStringToObject(resp, "message",
                              "timestamps must be second-precision UTC date-times");
      return resp;
   }
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "degraded");
      cJSON_AddStringToObject(resp, "channel", "semantic_assertion");
      cJSON_AddStringToObject(resp, "reason", "semantic retrieval unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "channel", "semantic_assertion");
   cJSON_AddStringToObject(resp, "mode", vector_available ? "hybrid_shadow" : "lexical_degraded");
   cJSON_AddStringToObject(resp, "channel_status", vector_available ? "ok" : "degraded");
   if (!vector_available)
      cJSON_AddStringToObject(resp, "degraded_reason", "embedding or vector index unavailable");
   cJSON_AddNumberToObject(resp, "max_hops", max_hops);
   cJSON_AddNumberToObject(resp, "indexed_assertions", indexed > 0 ? indexed : 0);
   cJSON *delta = cJSON_AddObjectToObject(resp, "shadow_delta");
   cJSON_AddNumberToObject(delta, "lexical_only", lexical_only);
   cJSON_AddNumberToObject(delta, "vector_only", vector_only);
   cJSON_AddNumberToObject(delta, "overlap", overlap);
   cJSON_AddStringToObject(resp, "valid_at", valid_at ? valid_at : "");
   cJSON_AddStringToObject(resp, "believed_at", believed_at ? believed_at : "");
   cJSON_AddBoolToObject(resp, "include_historical", include_historical);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_semantic_assertion_to_json(&hits[i]);
      if (obj)
         cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

static int kbs_typed_flag(const cJSON *req, const char *name, int fallback)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(req, name);
   if (!value)
      return fallback ? 1 : 0;
   return cJSON_IsBool(value) && cJSON_IsTrue(value);
}

static int kbs_typed_budget(const cJSON *req, const char *name, int fallback)
{
   const cJSON *budgets = cJSON_GetObjectItemCaseSensitive(req, "channel_budgets");
   const cJSON *value =
       cJSON_IsObject(budgets) ? cJSON_GetObjectItemCaseSensitive(budgets, name) : NULL;
   int result = cJSON_IsNumber(value) ? (int)value->valuedouble : fallback;
   if (result < 0)
      result = 0;
   if (result > 4096)
      result = 4096;
   return result;
}

static int kbs_estimate_tokens(const char *text)
{
   return text && text[0] ? (int)(strlen(text) / 4) + 1 : 0;
}

static void kbs_pack_trace(cJSON *trace, const char *channel, const char *stable_id, int tokens,
                           int included, const char *reason)
{
   cJSON *row = cJSON_CreateObject();
   if (!row)
      return;
   cJSON_AddStringToObject(row, "channel", channel);
   cJSON_AddStringToObject(row, "stable_id", stable_id ? stable_id : "");
   cJSON_AddNumberToObject(row, "estimated_tokens", tokens);
   cJSON_AddStringToObject(row, "decision", included ? "included" : "dropped");
   cJSON_AddStringToObject(row, "reason", reason ? reason : "");
   cJSON_AddItemToArray(trace, row);
}

static cJSON *kbs_typed_channel(cJSON *channels, const char *name, int enabled, int budget)
{
   cJSON *channel = cJSON_AddObjectToObject(channels, name);
   if (!channel)
      return NULL;
   cJSON_AddBoolToObject(channel, "enabled", enabled);
   cJSON_AddNumberToObject(channel, "budget_tokens", budget);
   cJSON_AddNumberToObject(channel, "used_tokens", 0);
   cJSON_AddStringToObject(channel, "status", enabled ? "ok" : "disabled");
   cJSON_AddArrayToObject(channel, "items");
   return channel;
}

static int kbs_channel_try_add(cJSON *channel, cJSON *trace, const char *channel_name,
                               const char *stable_id, cJSON *item, const char *rendered, int *used,
                               int budget, int *total_used, int total_budget)
{
   int tokens = kbs_estimate_tokens(rendered);
   int include = *used + tokens <= budget && *total_used + tokens <= total_budget;
   kbs_pack_trace(trace, channel_name, stable_id, tokens, include,
                  include ? "ranked evidence fit channel and total budgets"
                          : "channel or total token budget exhausted");
   if (!include)
   {
      cJSON_Delete(item);
      return 0;
   }
   cJSON *items = cJSON_GetObjectItemCaseSensitive(channel, "items");
   cJSON_AddItemToArray(items, item);
   *used += tokens;
   *total_used += tokens;
   cJSON_ReplaceItemInObjectCaseSensitive(channel, "used_tokens", cJSON_CreateNumber(*used));
   return 1;
}

static void kbs_typed_watermarks(cJSON *resp, const cJSON *req)
{
   char durable[40] = "";
   char observations[40] = "";
   void *conn = db2_conn();
   char err[256] = "";
   if (conn)
   {
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn,
                           "SELECT COALESCE(MAX(ts),'') FROM ("
                           " SELECT asserted_at AS ts FROM entity_edges WHERE edge_class='semantic'"
                           " UNION ALL SELECT created_at AS ts FROM memory_episodes) q",
                           err, sizeof(err));
      if (st && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         snprintf(durable, sizeof(durable), "%s", aimee_pg_column_text(st, 0));
      if (st)
         aimee_pg_finalize(st);
      st = aimee_pg_prepare(conn,
                            "SELECT COALESCE(MAX(refreshed_at),'')"
                            " FROM learning_observations",
                            err, sizeof(err));
      if (st && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         snprintf(observations, sizeof(observations), "%s", aimee_pg_column_text(st, 0));
      if (st)
         aimee_pg_finalize(st);
   }
   const cJSON *latest_j = cJSON_GetObjectItemCaseSensitive(req, "latest_turn_at");
   const char *latest = cJSON_IsString(latest_j) ? latest_j->valuestring : "";
   cJSON *watermark = cJSON_AddObjectToObject(resp, "watermark");
   cJSON_AddStringToObject(watermark, "durable_at", durable);
   cJSON_AddStringToObject(watermark, "observations_at", observations);
   cJSON_AddStringToObject(watermark, "latest_turn_at", latest);
   cJSON_AddBoolToObject(watermark, "durable_lag",
                         latest[0] && (!durable[0] || strcmp(durable, latest) < 0));
   cJSON_AddStringToObject(watermark, "status", durable[0] ? "known" : "unknown");
}

static int kbs_observation_visible(const learning_observation_t *obs,
                                   const db2_memory_scope_context_t *scope)
{
   if (scope->include_all)
      return 1;
   if (strcmp(obs->scope_kind, "global") == 0)
      return 1;
   if (strcmp(obs->scope_kind, "workspace") == 0)
      return scope->workspace[0] && strcmp(obs->scope_id, scope->workspace) == 0;
   if (strcmp(obs->scope_kind, "project") == 0)
      return scope->project[0] && strcmp(obs->scope_id, scope->project) == 0;
   return 0;
}

static int kbs_action_visible(const cJSON *action, const db2_memory_scope_context_t *scope)
{
   if (scope->include_all)
      return 1;
   const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(action, "scope_kind");
   const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(action, "scope_id");
   const char *kind = cJSON_IsString(kind_j) ? kind_j->valuestring : "";
   const char *id = cJSON_IsString(id_j) ? id_j->valuestring : "";
   if (strcmp(kind, "global") == 0)
      return 1;
   if (strcmp(kind, "workspace") == 0)
      return scope->workspace[0] && strcmp(id, scope->workspace) == 0;
   if (strcmp(kind, "project") == 0)
      return scope->project[0] && strcmp(id, scope->project) == 0;
   return 0;
}

cJSON *db2_kb_service_memory_assemble_typed_context_json(const cJSON *req)
{
   if (!req)
      return NULL;
   const cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   const char *query = cJSON_IsString(query_j) ? query_j->valuestring : "";
   const char *valid_at = "";
   const char *believed_at = "";
   const cJSON *valid_j = cJSON_GetObjectItemCaseSensitive(req, "valid_at");
   const cJSON *believed_j = cJSON_GetObjectItemCaseSensitive(req, "believed_at");
   if (cJSON_IsString(valid_j))
      valid_at = valid_j->valuestring;
   if (cJSON_IsString(believed_j))
      believed_at = believed_j->valuestring;

   /* The reviewed temporal-learning channels are the normal path. Callers retain
    * an explicit false opt-out for the master assembler and for each channel.
    * Historical recall remains opt-in: default-on retrieval still means the
    * safe current view, never an implicit mixture of old and current facts. */
   int enabled = kbs_typed_flag(req, "enabled", 1);
   int semantic_enabled = enabled && kbs_typed_flag(req, "enable_semantic_assertions", 1);
   int historical_enabled = semantic_enabled && kbs_typed_flag(req, "enable_historical", 0);
   int episodes_enabled = enabled && kbs_typed_flag(req, "enable_episodes", 0);
   int summaries_enabled = enabled && kbs_typed_flag(req, "enable_summaries", 0);
   int observations_enabled = enabled && kbs_typed_flag(req, "enable_observations", 1);
   int procedures_enabled = enabled && kbs_typed_flag(req, "enable_approved_procedures", 1);
   int working_enabled = enabled && kbs_typed_flag(req, "enable_working_context", 0);
   int total_budget = kbs_typed_budget(req, "total", 2400);
   int total_used = 0;

   cJSON *resp = cJSON_CreateObject();
   cJSON *channels = resp ? cJSON_AddObjectToObject(resp, "channels") : NULL;
   cJSON *trace = resp ? cJSON_AddArrayToObject(resp, "packing_trace") : NULL;
   if (!resp || !channels || !trace)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "default_injection", enabled);
   cJSON_AddNumberToObject(resp, "total_budget_tokens", total_budget);

   int current_budget = kbs_typed_budget(req, "current_assertions", 800);
   int historical_budget = kbs_typed_budget(req, "historical_assertions", 400);
   int episodes_budget = kbs_typed_budget(req, "episodes", 500);
   int summaries_budget = kbs_typed_budget(req, "summaries", 300);
   int observations_budget = kbs_typed_budget(req, "observations", 500);
   int procedures_budget = kbs_typed_budget(req, "approved_procedures", 500);
   int working_budget = kbs_typed_budget(req, "working_context", 500);
   cJSON *current_ch =
       kbs_typed_channel(channels, "current_assertions", semantic_enabled, current_budget);
   cJSON *historical_ch =
       kbs_typed_channel(channels, "historical_assertions", historical_enabled, historical_budget);
   cJSON *episodes_ch = kbs_typed_channel(channels, "episodes", episodes_enabled, episodes_budget);
   cJSON *summaries_ch =
       kbs_typed_channel(channels, "summaries", summaries_enabled, summaries_budget);
   cJSON *observations_ch =
       kbs_typed_channel(channels, "observations", observations_enabled, observations_budget);
   cJSON *procedures_ch =
       kbs_typed_channel(channels, "approved_procedures", procedures_enabled, procedures_budget);
   cJSON *working_ch =
       kbs_typed_channel(channels, "working_context", working_enabled, working_budget);
   int current_used = 0, historical_used = 0, episodes_used = 0, summaries_used = 0;
   int observations_used = 0, procedures_used = 0, working_used = 0;
   int included_count = 0;
   int degraded = 0;

   if (semantic_enabled)
   {
      semantic_assertion_hit_t hits[32];
      int vector_available = 0, indexed = 0, lexical_only = 0, vector_only = 0, overlap = 0;
      int hops = 0;
      const cJSON *hops_j = cJSON_GetObjectItemCaseSensitive(req, "max_hops");
      if (cJSON_IsNumber(hops_j))
         hops = (int)hops_j->valuedouble;
      if (hops < 0)
         hops = 0;
      if (hops > 2)
         hops = 2;
      int n = kbs_semantic_assertion_hybrid(query, valid_at, believed_at, historical_enabled, hops,
                                            hits, 32, &vector_available, &indexed, &lexical_only,
                                            &vector_only, &overlap);
      if (n == SEMANTIC_ASSERTION_SEARCH_INVALID_TIME)
      {
         cJSON_ReplaceItemInObjectCaseSensitive(resp, "status", cJSON_CreateString("error"));
         cJSON_AddStringToObject(resp, "error_type", "invalid_timestamp");
         cJSON_AddStringToObject(resp, "message",
                                 "timestamps must be second-precision UTC date-times");
         cJSON_ReplaceItemInObjectCaseSensitive(current_ch, "status", cJSON_CreateString("error"));
         cJSON_AddStringToObject(current_ch, "reason", "invalid temporal request");
         kbs_typed_watermarks(resp, req);
         cJSON_AddNumberToObject(resp, "used_tokens", 0);
         cJSON_AddStringToObject(resp, "context_sufficiency", "insufficient");
         cJSON_AddStringToObject(resp, "sufficiency_reason",
                                 "invalid temporal request; no context assembled");
         return resp;
      }
      if (n < 0)
      {
         degraded = 1;
         cJSON_ReplaceItemInObjectCaseSensitive(current_ch, "status",
                                                cJSON_CreateString("degraded"));
         cJSON_AddStringToObject(current_ch, "reason", "semantic retrieval unavailable");
      }
      else
      {
         if (!vector_available)
         {
            degraded = 1;
            cJSON_ReplaceItemInObjectCaseSensitive(current_ch, "status",
                                                   cJSON_CreateString("degraded"));
            cJSON_AddStringToObject(current_ch, "reason", "lexical fallback; vector unavailable");
         }
         for (int i = 0; i < n; i++)
         {
            cJSON *item = kbs_semantic_assertion_to_json(&hits[i]);
            char rendered[1200];
            snprintf(rendered, sizeof(rendered), "%s %s %s%s", hits[i].subject, hits[i].relation,
                     hits[i].object, hits[i].historical ? " [HISTORICAL]" : "");
            char stable_id[64];
            snprintf(stable_id, sizeof(stable_id), "%lld", (long long)hits[i].assertion_id);
            if (hits[i].historical)
            {
               if (historical_enabled)
                  included_count += kbs_channel_try_add(
                      historical_ch, trace, "historical_assertions", stable_id, item, rendered,
                      &historical_used, historical_budget, &total_used, total_budget);
               else
               {
                  kbs_pack_trace(trace, "historical_assertions", stable_id,
                                 kbs_estimate_tokens(rendered), 0,
                                 "historical channel explicitly disabled");
                  cJSON_Delete(item);
               }
            }
            else
               included_count += kbs_channel_try_add(current_ch, trace, "current_assertions",
                                                     stable_id, item, rendered, &current_used,
                                                     current_budget, &total_used, total_budget);
         }
      }
   }

   if (episodes_enabled)
   {
      memory_episode_t episodes[16];
      int n = memory_list_episodes(query, 16, episodes, 16);
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_CreateObject();
         char stable_id[64];
         snprintf(stable_id, sizeof(stable_id), "%lld", (long long)episodes[i].id);
         cJSON_AddStringToObject(item, "stable_id", stable_id);
         cJSON_AddStringToObject(item, "episode_key", episodes[i].episode_key);
         cJSON_AddStringToObject(item, "excerpt", episodes[i].episode_text);
         cJSON_AddStringToObject(item, "source_session", episodes[i].source_session);
         cJSON_AddStringToObject(item, "reference_time", episodes[i].reference_time);
         cJSON_AddStringToObject(item, "trust", "untrusted_data");
         included_count += kbs_channel_try_add(episodes_ch, trace, "episodes", stable_id, item,
                                               episodes[i].episode_text, &episodes_used,
                                               episodes_budget, &total_used, total_budget);
      }
   }

   if (summaries_enabled)
   {
      memory_entity_profile_t profile;
      if (memory_get_entity_profile(query, &profile) == 0 && profile.summary[0])
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "entity", profile.entity);
         cJSON_AddStringToObject(item, "summary", profile.summary);
         cJSON_AddStringToObject(item, "authority", "derived_noncanonical");
         included_count += kbs_channel_try_add(summaries_ch, trace, "summaries", profile.entity,
                                               item, profile.summary, &summaries_used,
                                               summaries_budget, &total_used, total_budget);
      }
   }

   db2_memory_scope_context_t scope;
   db2_memory_scope_context_get(&scope);
   if (observations_enabled)
   {
      learning_observation_t observations[64];
      int n = db2_learning_observation_list("active", NULL, NULL, 64, observations, 64);
      if (n < 0)
      {
         degraded = 1;
         cJSON_ReplaceItemInObjectCaseSensitive(observations_ch, "status",
                                                cJSON_CreateString("degraded"));
         cJSON_AddStringToObject(observations_ch, "reason", "observation synthesis unavailable");
      }
      for (int i = 0; i < n; i++)
      {
         if (!kbs_observation_visible(&observations[i], &scope))
         {
            kbs_pack_trace(trace, "observations", observations[i].observation_id,
                           kbs_estimate_tokens(observations[i].summary), 0,
                           "deny-dominant scope inheritance");
            continue;
         }
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "observation_id", observations[i].observation_id);
         cJSON_AddStringToObject(item, "type", observations[i].observation_type);
         cJSON_AddStringToObject(item, "title", observations[i].title);
         cJSON_AddStringToObject(item, "summary", observations[i].summary);
         cJSON_AddNumberToObject(item, "confidence", observations[i].confidence);
         cJSON_AddNumberToObject(item, "evidence_count", observations[i].evidence_count);
         cJSON_AddStringToObject(item, "authority", "derived_read_only");
         included_count += kbs_channel_try_add(observations_ch, trace, "observations",
                                               observations[i].observation_id, item,
                                               observations[i].summary, &observations_used,
                                               observations_budget, &total_used, total_budget);
      }
   }

   if (procedures_enabled)
   {
      learning_proposal_t proposals[32];
      int n = db2_learning_proposal_list("committed", "artifact", 32, proposals, 32);
      if (n < 0)
      {
         degraded = 1;
         cJSON_ReplaceItemInObjectCaseSensitive(procedures_ch, "status",
                                                cJSON_CreateString("degraded"));
         cJSON_AddStringToObject(procedures_ch, "reason", "reviewed procedure store unavailable");
      }
      for (int i = 0; i < n; i++)
      {
         cJSON *action = cJSON_Parse(proposals[i].action_json);
         if (!action || !kbs_action_visible(action, &scope))
         {
            kbs_pack_trace(trace, "approved_procedures", proposals[i].target_key,
                           kbs_estimate_tokens(proposals[i].action_json), 0,
                           "procedure scope not visible to caller");
            cJSON_Delete(action);
            continue;
         }
         cJSON *item = cJSON_CreateObject();
         cJSON_AddNumberToObject(item, "proposal_id", proposals[i].id);
         cJSON_AddStringToObject(item, "target_key", proposals[i].target_key);
         cJSON_AddStringToObject(item, "state", "committed");
         cJSON_AddItemToObject(item, "procedure", action);
         included_count +=
             kbs_channel_try_add(procedures_ch, trace, "approved_procedures",
                                 proposals[i].target_key, item, proposals[i].action_json,
                                 &procedures_used, procedures_budget, &total_used, total_budget);
      }
   }

   if (working_enabled)
   {
      const cJSON *turns = cJSON_GetObjectItemCaseSensitive(req, "recent_turns");
      if (cJSON_IsArray(turns))
      {
         const cJSON *turn = NULL;
         int index = 0;
         cJSON_ArrayForEach(turn, turns)
         {
            if (!cJSON_IsString(turn))
               continue;
            cJSON *item = cJSON_CreateObject();
            char stable_id[32];
            snprintf(stable_id, sizeof(stable_id), "turn:%d", index++);
            cJSON_AddStringToObject(item, "stable_id", stable_id);
            cJSON_AddStringToObject(item, "text", turn->valuestring);
            cJSON_AddStringToObject(item, "authority", "ephemeral_untrusted_data");
            included_count += kbs_channel_try_add(working_ch, trace, "working_context", stable_id,
                                                  item, turn->valuestring, &working_used,
                                                  working_budget, &total_used, total_budget);
         }
      }
   }

   kbs_typed_watermarks(resp, req);
   cJSON_AddNumberToObject(resp, "used_tokens", total_used);
   const char *sufficiency = included_count == 0 ? "insufficient"
                             : degraded          ? "partial"
                                                 : "complete";
   cJSON_AddStringToObject(resp, "context_sufficiency", sufficiency);
   cJSON_AddStringToObject(
       resp, "sufficiency_reason",
       included_count == 0 ? "no authorized evidence fit enabled channels"
       : degraded          ? "authorized evidence present but a requested channel degraded"
                           : "authorized evidence present in every available requested channel");

   /* The renderer preserves the trust boundary explicitly. Default ingress
    * injection consumes it, while callers can still disable the assembler or
    * individual channels on a request. */
   char *channels_json = cJSON_PrintUnformatted(channels);
   cJSON *procedure_items = cJSON_GetObjectItemCaseSensitive(procedures_ch, "items");
   char *procedures_json = cJSON_PrintUnformatted(procedure_items);
   if (channels_json)
   {
      size_t cap = strlen(channels_json) + (procedures_json ? strlen(procedures_json) : 2) + 512;
      char *rendered = malloc(cap);
      if (rendered)
      {
         snprintf(rendered, cap,
                  "<memory_data trust=\"untrusted\" authorization=\"none\">%s</memory_data>\n"
                  "<approved_procedures authority=\"reviewed\" authorization=\"none\">%s"
                  "</approved_procedures>",
                  channels_json, procedures_json ? procedures_json : "[]");
         cJSON_AddStringToObject(resp, "rendered_context", rendered);
         free(rendered);
      }
      free(channels_json);
   }
   free(procedures_json);
   return resp;
}
