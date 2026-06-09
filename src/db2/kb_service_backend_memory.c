/* db2/kb_service_backend_memory.c: memory-domain RPC backends for
 * aimee-kb (find_facts, list, get, insert, briefing, context_block,
 * entity_profile, entity_edges).  Split from kb_service_backend.c so
 * the two stay under the per-file line cap. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "memory_lint.h"
#include "config.h"
#include "decision_log.h"
#include "memory.h"
#include "memory_export.h"
#include "memory_payload.h"
#include "memory_query.h"
#include "session_briefing.h"
#include "tasks.h"

#include <stdlib.h>
#include <string.h>

static cJSON *kbs_memory_row_to_json(const memory_t *m)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)m->id);
   cJSON_AddStringToObject(obj, "tier", m->tier);
   cJSON_AddStringToObject(obj, "kind", m->kind);
   cJSON_AddStringToObject(obj, "key", m->key);
   db2_memory_summary_row_t summaries[4];
   int summary_n = db2_memory_summaries_list(m->id, 4, summaries, 4);
   const char *headline = "";
   for (int i = 0; i < summary_n; i++)
      if (strcmp(summaries[i].scope, "headline") == 0 && summaries[i].summary[0])
      {
         headline = summaries[i].summary;
         break;
      }
   if (!headline[0] && summary_n > 0)
      headline = summaries[0].summary;
   cJSON_AddStringToObject(obj, "headline", headline);
   cJSON_AddStringToObject(obj, "content", m->content);
   cJSON_AddStringToObject(obj, "use_cases", m->use_cases);
   cJSON_AddNumberToObject(obj, "confidence", m->confidence);
   cJSON_AddNumberToObject(obj, "use_count", m->use_count);
   cJSON_AddStringToObject(obj, "last_used_at", m->last_used_at);
   cJSON_AddStringToObject(obj, "created_at", m->created_at);
   cJSON_AddStringToObject(obj, "updated_at", m->updated_at);
   cJSON_AddStringToObject(obj, "source_session", m->source_session);
   return obj;
}

cJSON *db2_kb_service_memory_find_facts_json(const char *query, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "facts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_t facts[64];
   int n = memory_find_facts(query ? query : "", limit, facts, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&facts[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_list_json(const char *tier, const char *kind, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "memories") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[64];
   int n = memory_list(tier, kind, limit, rows, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

static cJSON *kbs_prospective_to_json(const memory_prospective_t *r)
{
   cJSON *j = cJSON_CreateObject();
   if (!j)
      return NULL;
   cJSON_AddNumberToObject(j, "id", (double)r->id);
   cJSON_AddStringToObject(j, "trigger_text", r->trigger_text);
   cJSON_AddStringToObject(j, "action_text", r->action_text);
   cJSON_AddStringToObject(j, "anchor_entity", r->anchor_entity);
   cJSON_AddStringToObject(j, "anchor_file", r->anchor_file);
   cJSON_AddStringToObject(j, "recurrence", r->recurrence);
   cJSON_AddStringToObject(j, "state", r->state);
   cJSON_AddStringToObject(j, "valid_until", r->valid_until);
   cJSON_AddNumberToObject(j, "trigger_count", r->trigger_count);
   cJSON_AddStringToObject(j, "last_triggered_at", r->last_triggered_at);
   cJSON_AddStringToObject(j, "created_at", r->created_at);
   cJSON_AddStringToObject(j, "updated_at", r->updated_at);
   return j;
}

cJSON *db2_kb_service_memory_prospective_list_json(const char *state, int max)
{
   if (max < 1)
      max = 50;
   if (max > 256)
      max = 256;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "prospectives") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   memory_prospective_t rows[256];
   int n = memory_prospective_list(state, rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *r = kbs_prospective_to_json(&rows[i]);
      if (r)
         cJSON_AddItemToArray(arr, r);
   }
   return resp;
}

cJSON *
db2_kb_service_memory_prospective_create_json(const char *trigger_text, const char *action_text,
                                              const char *anchor_entity, const char *anchor_file,
                                              const char *recurrence, const char *valid_until)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_prospective_t row;
   int rc =
       memory_prospective_create(trigger_text ? trigger_text : "", action_text ? action_text : "",
                                 anchor_entity ? anchor_entity : "", anchor_file ? anchor_file : "",
                                 recurrence, valid_until ? valid_until : "", "", &row);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "create failed (check recurrence value)");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *j = kbs_prospective_to_json(&row);
   if (j)
      cJSON_AddItemToObject(resp, "prospective", j);
   return resp;
}

cJSON *db2_kb_service_memory_list_conflicts_json(int max)
{
   if (max < 1)
      max = 64;
   if (max > 256)
      max = 256;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "conflicts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   conflict_t conflicts[256];
   int n = memory_list_conflicts(conflicts, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", (double)conflicts[i].id);
      cJSON_AddNumberToObject(obj, "memory_a", (double)conflicts[i].memory_a);
      cJSON_AddNumberToObject(obj, "memory_b", (double)conflicts[i].memory_b);
      cJSON_AddStringToObject(obj, "detected_at", conflicts[i].detected_at);
      cJSON_AddNumberToObject(obj, "resolved", conflicts[i].resolved);
      cJSON_AddStringToObject(obj, "resolution", conflicts[i].resolution);
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

static cJSON *kbs_memory_diagnostic_to_json(const memory_diagnostic_t *d)
{
   cJSON *j = cJSON_CreateObject();
   if (!j)
      return NULL;
   cJSON *mem = kbs_memory_row_to_json(&d->memory);
   if (!mem)
   {
      cJSON_Delete(j);
      return NULL;
   }
   cJSON_AddItemToObject(j, "memory", mem);
   cJSON *parts = cJSON_AddObjectToObject(j, "parts");
   cJSON_AddNumberToObject(parts, "lexical", d->parts.lexical);
   cJSON_AddNumberToObject(parts, "coverage", d->parts.coverage);
   cJSON_AddNumberToObject(parts, "entity", d->parts.entity);
   cJSON_AddNumberToObject(parts, "temporal", d->parts.temporal);
   cJSON_AddNumberToObject(parts, "evidence", d->parts.evidence);
   cJSON_AddNumberToObject(parts, "semantic", d->parts.semantic);
   cJSON_AddNumberToObject(parts, "state", d->parts.state);
   cJSON_AddNumberToObject(parts, "intent", d->parts.intent);
   cJSON_AddNumberToObject(parts, "confidence", d->parts.confidence);
   cJSON_AddNumberToObject(parts, "salience", d->parts.salience);
   cJSON_AddNumberToObject(parts, "surprise", d->parts.surprise);
   cJSON_AddNumberToObject(parts, "pagerank", d->parts.pagerank);
   cJSON_AddNumberToObject(parts, "cross_encoder", d->parts.cross_encoder);
   cJSON_AddNumberToObject(parts, "hybrid_total", d->parts.hybrid_total);
   cJSON_AddNumberToObject(parts, "blended_total", d->parts.blended_total);
   cJSON_AddNumberToObject(parts, "rerank_mix", d->parts.rerank_mix);
   cJSON_AddNumberToObject(parts, "total", d->parts.total);
   return j;
}

cJSON *db2_kb_service_memory_diagnose_scoped_json(const char *query, const char *scope_type,
                                                  const char *scope_value, int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "rows") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_diagnostic_t rows[64];
   int n = memory_diagnose_scoped(query ? query : "", scope_type, scope_value, limit, rows, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_diagnostic_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_explain_match_json(const char *query, int64_t memory_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_diagnostic_t row;
   memset(&row, 0, sizeof(row));
   if (memory_explain_match(query ? query : "", memory_id, &row) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              "explain failed (memory retrieval unavailable or id missing)");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *row_j = kbs_memory_diagnostic_to_json(&row);
   if (row_j)
      cJSON_AddItemToObject(resp, "row", row_j);
   return resp;
}

cJSON *db2_kb_service_memory_find_facts_visible_json(const char *query, const char *workspace,
                                                     const char *project, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "facts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_t facts[64];
   int n = memory_find_facts_visible(query ? query : "", workspace, project, limit, facts, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&facts[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_find_facts_scoped_json(const char *query, const char *scope_type,
                                                    const char *scope_value, int limit)
{
   if (limit < 1)
      limit = 20;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "facts") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }

   memory_t facts[64];
   int n = memory_find_facts_scoped(query ? query : "", scope_type ? scope_type : "",
                                    scope_value ? scope_value : "", limit, facts, 64);
   if (n < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory retrieval index unavailable");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&facts[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_export_jsonl_json(const char *path)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!path || !path[0])
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing path");
      return resp;
   }
   db2_memory_export_row_t *rows = NULL;
   size_t row_count = 0;
   if (db2_memory_export_alloc_all(&rows, &row_count) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory_export_alloc_all failed");
      return resp;
   }

   FILE *f = fopen(path, "w");
   if (!f)
   {
      for (size_t i = 0; i < row_count; i++)
         db2_memory_export_row_free(&rows[i]);
      free(rows);
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "could not open output file");
      return resp;
   }

   int count = 0;
   for (size_t ri = 0; ri < row_count; ri++)
   {
      db2_memory_export_row_t *r = &rows[ri];
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", (double)r->id);
      cJSON_AddStringToObject(obj, "tier", r->tier);
      cJSON_AddStringToObject(obj, "kind", r->kind);
      cJSON_AddStringToObject(obj, "key", r->key ? r->key : "");
      cJSON_AddStringToObject(obj, "content", r->content ? r->content : "");
      cJSON_AddNumberToObject(obj, "confidence", r->confidence);
      cJSON_AddNumberToObject(obj, "use_count", r->use_count);
      cJSON_AddStringToObject(obj, "source_session", r->source_session ? r->source_session : "");
      cJSON_AddStringToObject(obj, "created_at", r->created_at ? r->created_at : "");
      cJSON_AddStringToObject(obj, "updated_at", r->updated_at ? r->updated_at : "");

      memory_scope_tag_t scopes[8];
      int scope_count = memory_collect_scopes(r->id, scopes, 8);
      cJSON *scope_arr = cJSON_AddArrayToObject(obj, "scopes");
      for (int i = 0; i < scope_count; i++)
      {
         cJSON *scope = cJSON_CreateObject();
         cJSON_AddStringToObject(scope, "type", scopes[i].type);
         cJSON_AddStringToObject(scope, "value", scopes[i].value);
         cJSON_AddItemToArray(scope_arr, scope);
      }

      char primary_value[128];
      memory_scope_level_t primary_level =
          memory_primary_scope(r->id, primary_value, sizeof(primary_value));
      cJSON *primary = cJSON_AddObjectToObject(obj, "primary_scope");
      cJSON_AddStringToObject(primary, "type", memory_scope_level_name(primary_level));
      cJSON_AddStringToObject(primary, "value", primary_value);

      char *line = cJSON_PrintUnformatted(obj);
      cJSON_Delete(obj);
      if (line)
      {
         fprintf(f, "%s\n", line);
         free(line);
         count++;
      }
   }
   fclose(f);

   for (size_t i = 0; i < row_count; i++)
      db2_memory_export_row_free(&rows[i]);
   free(rows);

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", count);
   return resp;
}

cJSON *db2_kb_service_memory_decisions_export_jsonl_json(const char *path)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!path || !path[0])
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "missing path");
      return resp;
   }
   int rc = db2_memory_decisions_export_jsonl(path);
   if (rc < 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "decisions export failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "count", rc);
   return resp;
}

cJSON *db2_kb_service_memory_key_exists_json(const char *key)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int exists = db2_memory_key_exists(key ? key : "") ? 1 : 0;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "exists", exists);
   return resp;
}

cJSON *db2_kb_service_memory_search_json(const cJSON *clusters_arr, int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *out_arr = resp ? cJSON_AddArrayToObject(resp, "results") : NULL;
   if (!resp || !out_arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   /* Materialize cluster strings into a stable char** for memory_search. */
   char *clusters[64];
   int cluster_count = 0;
   if (cJSON_IsArray(clusters_arr))
   {
      cJSON *c;
      cJSON_ArrayForEach(c, clusters_arr)
      {
         if (cluster_count >= 64)
            break;
         if (cJSON_IsString(c) && c->valuestring[0])
            clusters[cluster_count++] = c->valuestring;
      }
   }

   search_result_t *rows = calloc((size_t)limit, sizeof(search_result_t));
   if (!rows)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   int n = memory_search(clusters, cluster_count, limit, rows, limit);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "session_id", rows[i].session_id);
      cJSON_AddNumberToObject(obj, "seq", rows[i].seq);
      cJSON_AddStringToObject(obj, "file_path", rows[i].file_path);
      cJSON_AddNumberToObject(obj, "start_line", rows[i].start_line);
      cJSON_AddNumberToObject(obj, "end_line", rows[i].end_line);
      cJSON_AddStringToObject(obj, "summary", rows[i].summary);
      cJSON_AddNumberToObject(obj, "score", rows[i].score);
      cJSON *files = cJSON_AddArrayToObject(obj, "files");
      for (int f = 0; f < rows[i].file_count && f < 32; f++)
         cJSON_AddItemToArray(files, cJSON_CreateString(rows[i].files[f]));
      cJSON_AddItemToArray(out_arr, obj);
   }
   free(rows);
   return resp;
}

cJSON *db2_kb_service_memory_assemble_context_json(const char *task_hint)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = memory_assemble_context(task_hint);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "context", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_memory_compact_windows_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int summaries = 0, facts = 0;
   memory_compact_windows(&summaries, &facts);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "summaries", summaries);
   cJSON_AddNumberToObject(resp, "facts", facts);
   return resp;
}

cJSON *db2_kb_service_memory_query_edges_json(const char *entity, int max)
{
   if (max < 1)
      max = 128;
   if (max > 256)
      max = 256;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "edges") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   edge_t edges[256];
   int n = memory_query_edges(entity ? entity : "", edges, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", (double)edges[i].id);
      cJSON_AddStringToObject(obj, "source", edges[i].source);
      cJSON_AddStringToObject(obj, "relation", edges[i].relation);
      cJSON_AddStringToObject(obj, "target", edges[i].target);
      cJSON_AddNumberToObject(obj, "weight", edges[i].weight);
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_effectiveness_stats_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   effectiveness_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   if (memory_effectiveness_stats(&stats) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory_effectiveness_stats failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *s = cJSON_AddObjectToObject(resp, "stats");
   cJSON_AddNumberToObject(s, "avg_effectiveness", stats.avg_effectiveness);
   cJSON_AddNumberToObject(s, "low_effectiveness_count", stats.low_effectiveness_count);
   cJSON_AddNumberToObject(s, "high_impact_count", stats.high_impact_count);
   cJSON_AddNumberToObject(s, "never_surfaced_l2", stats.never_surfaced_l2);
   return resp;
}

cJSON *db2_kb_service_memory_query_health_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_health_t health;
   memset(&health, 0, sizeof(health));
   int rc = memory_query_health(&health);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory_query_health failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *h = cJSON_AddObjectToObject(resp, "health");
   cJSON_AddNumberToObject(h, "cycles", health.cycles);
   cJSON_AddNumberToObject(h, "contradiction_rate", health.contradiction_rate);
   cJSON_AddNumberToObject(h, "promotion_rate", health.promotion_rate);
   cJSON_AddNumberToObject(h, "demotion_rate", health.demotion_rate);
   cJSON_AddNumberToObject(h, "staleness", health.staleness);
   cJSON_AddNumberToObject(h, "total_contradictions", health.total_contradictions);
   cJSON_AddNumberToObject(h, "total_promotions", health.total_promotions);
   cJSON_AddNumberToObject(h, "total_demotions", health.total_demotions);
   cJSON_AddNumberToObject(h, "total_expirations", health.total_expirations);
   return resp;
}

cJSON *db2_kb_service_memory_delete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (memory_delete(id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete memory");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_touch_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (memory_touch(id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to touch memory");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_update_json(int64_t id, const char *content)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (!content || !content[0])
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "content is required");
      return resp;
   }
   if (memory_update_content(id, content) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to update memory content");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_reject_json(int64_t id, const char *reason)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (memory_reject(id, reason) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to reject memory");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_stats_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_stats_t stats;
   memset(&stats, 0, sizeof(stats));
   int rc = memory_stats(&stats);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory_stats failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *s = cJSON_AddObjectToObject(resp, "stats");
   cJSON_AddNumberToObject(s, "total", stats.total);
   cJSON_AddNumberToObject(s, "conflicts", stats.conflicts);
   cJSON_AddNumberToObject(s, "pagerank_last_ms", stats.pagerank_last_ms);
   cJSON_AddNumberToObject(s, "pagerank_avg_ms", stats.pagerank_avg_ms);
   cJSON_AddNumberToObject(s, "pagerank_max_ms", stats.pagerank_max_ms);
   cJSON_AddNumberToObject(s, "pagerank_samples", stats.pagerank_samples);
   cJSON_AddNumberToObject(s, "pagerank_last_candidates", stats.pagerank_last_candidates);
   cJSON_AddNumberToObject(s, "pagerank_last_edges", stats.pagerank_last_edges);
   cJSON *tiers = cJSON_AddArrayToObject(s, "tier_counts");
   for (int i = 0; i < 6; i++)
      cJSON_AddItemToArray(tiers, cJSON_CreateNumber(stats.tier_counts[i]));
   cJSON *kinds = cJSON_AddArrayToObject(s, "kind_counts");
   for (int i = 0; i < KIND_COUNT; i++)
      cJSON_AddItemToArray(kinds, cJSON_CreateNumber(stats.kind_counts[i]));
   return resp;
}

cJSON *db2_kb_service_memory_link_create_json(int64_t source_id, int64_t target_id,
                                              const char *relation)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = memory_link_create(source_id, target_id, relation ? relation : "");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to create link");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_link_query_json(int64_t memory_id, int max)
{
   if (max < 1)
      max = 32;
   if (max > 64)
      max = 64;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "links") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   memory_link_t links[64];
   int n = memory_link_query(memory_id, links, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", (double)links[i].id);
      cJSON_AddNumberToObject(obj, "source_id", (double)links[i].source_id);
      cJSON_AddNumberToObject(obj, "target_id", (double)links[i].target_id);
      cJSON_AddStringToObject(obj, "relation", links[i].relation);
      cJSON_AddStringToObject(obj, "created_at", links[i].created_at);
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_link_delete_json(int64_t link_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (memory_link_delete(link_id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete link");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_upsert_workflow_json(const char *workspace, const char *signal_type,
                                                  const char *rule, double observed_confidence,
                                                  const char *session_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int64_t id =
       memory_upsert_workflow(workspace ? workspace : "", signal_type ? signal_type : "",
                              rule ? rule : "", observed_confidence, session_id ? session_id : "");
   if (id <= 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to store workflow memory");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", (double)id);
   return resp;
}

cJSON *db2_kb_service_memory_alerts_json(const char *since)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON *bundle = memory_alerts(since);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "alerts", bundle ? bundle : cJSON_CreateObject());
   return resp;
}

cJSON *db2_kb_service_memory_recall_json(const char *task_hint, int limit_tokens, int session_start)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON *bundle = memory_recall(task_hint, limit_tokens, session_start);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "recall", bundle ? bundle : cJSON_CreateObject());
   return resp;
}

cJSON *db2_kb_service_memory_maintenance_run_json(unsigned int modes, int force, int dry_run)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_load(&cfg);
   memory_maintenance_summary_t summary;
   memory_maintenance_run(&cfg, modes, force, dry_run, &summary);
   cJSON *summary_j = memory_maintenance_summary_to_json(&summary);
   cJSON_AddStringToObject(resp, "status", "ok");
   if (summary_j)
      cJSON_AddItemToObject(resp, "summary", summary_j);
   return resp;
}

cJSON *db2_kb_service_memory_prospective_sweep_expired_json(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int n = memory_prospective_sweep_expired();
   if (n < 0)
      n = 0;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "expired", n);
   return resp;
}

cJSON *db2_kb_service_memory_prospective_complete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   if (memory_prospective_complete(id) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "could not complete (terminal or missing)");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_episode_card_generate_json(const char *source_session)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_load(&cfg);
   int64_t uid = memory_episode_card_generate(source_session ? source_session : "", &cfg);
   cJSON_AddStringToObject(resp, "status", uid > 0 ? "ok" : "error");
   cJSON_AddNumberToObject(resp, "memory_unit_id", (double)uid);
   if (uid <= 0)
      cJSON_AddStringToObject(resp, "message", "episode card generation produced no row");
   return resp;
}

cJSON *db2_kb_service_memory_scope_visibility_rank_json(const int64_t *ids, int id_count,
                                                        const char *workspace, const char *project)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "ranks") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   for (int i = 0; i < id_count; i++)
   {
      int rank = memory_scope_visibility_rank(ids[i], workspace, project);
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(rank));
   }
   return resp;
}

cJSON *db2_kb_service_memory_tag_workspace_json(int64_t memory_id, const char *workspace)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = memory_tag_workspace(memory_id, workspace);
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc != 0)
      cJSON_AddStringToObject(resp, "message", "tag_workspace failed");
   return resp;
}

cJSON *db2_kb_service_memory_tag_scope_json(int64_t memory_id, const char *scope_type,
                                            const char *scope_value)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = memory_tag_scope(memory_id, scope_type, scope_value);
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc != 0)
      cJSON_AddStringToObject(resp, "message", "tag_scope failed");
   return resp;
}

cJSON *db2_kb_service_memory_get_provenance_json(int64_t memory_id, int max)
{
   if (max < 1 || max > MAX_PROVENANCE_ENTRIES)
      max = MAX_PROVENANCE_ENTRIES;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "entries") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   provenance_entry_t rows[MAX_PROVENANCE_ENTRIES];
   int n = memory_get_provenance(memory_id, rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddNumberToObject(r, "id", (double)rows[i].id);
      cJSON_AddNumberToObject(r, "memory_id", (double)rows[i].memory_id);
      cJSON_AddStringToObject(r, "session_id", rows[i].session_id);
      cJSON_AddStringToObject(r, "action", rows[i].action);
      cJSON_AddStringToObject(r, "details", rows[i].details);
      cJSON_AddStringToObject(r, "created_at", rows[i].created_at);
      cJSON_AddItemToArray(arr, r);
   }
   return resp;
}

cJSON *db2_kb_service_memory_prospective_match_json(const char *turn_text,
                                                    const char *active_entity,
                                                    const char *active_file, int max)
{
   if (max < 1)
      max = 3;
   if (max > MEMORY_PROSPECTIVE_MAX_MATCHES)
      max = MEMORY_PROSPECTIVE_MAX_MATCHES;
   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "matches") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   memory_prospective_t rows[MEMORY_PROSPECTIVE_MAX_MATCHES];
   int n = memory_prospective_match(turn_text, active_entity, active_file, rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *r = kbs_prospective_to_json(&rows[i]);
      if (r)
         cJSON_AddItemToArray(arr, r);
   }
   return resp;
}

cJSON *db2_kb_service_memory_prospective_mark_triggered_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = memory_prospective_mark_triggered(id);
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc != 0)
      cJSON_AddStringToObject(resp, "message", "mark_triggered failed");
   return resp;
}

cJSON *db2_kb_service_session_briefing_commitments_json(int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = session_briefing_render_commitments(limit);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_session_briefing_directives_json(int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   char *body = session_briefing_render_directives(limit);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "body", body ? body : "");
   free(body);
   return resp;
}

cJSON *db2_kb_service_memory_top_l2_facts_json(int max)
{
   if (max < 1)
      max = 5;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "memories") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[64];
   int n = db2_memory_top_l2_facts(rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_load_eval_corpus_json(int max)
{
   if (max < 1)
      max = 20;
   if (max > 100)
      max = 100;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "memories") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[100];
   char label[64] = "";
   int n = db2_memory_load_eval_corpus(rows, max, label, sizeof(label));
   cJSON_AddStringToObject(resp, "label", label);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_get_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   memory_t m;
   if (memory_get(id, &m) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory not found");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_memory_row_to_json(&m);
   if (!obj)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddItemToObject(resp, "memory", obj);
   return resp;
}

cJSON *db2_kb_service_memory_insert_json(const char *tier, const char *kind, const char *key,
                                         const char *content, double confidence,
                                         const char *session_id)
{
   return db2_kb_service_memory_insert_ex_json(tier, kind, key, content, "", confidence,
                                               session_id);
}

cJSON *db2_kb_service_memory_insert_ex_json(const char *tier, const char *kind, const char *key,
                                            const char *content, const char *use_cases,
                                            double confidence, const char *session_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   memory_t out;
   int rc =
       memory_insert_ex(tier ? tier : "", kind ? kind : "", key ? key : "", content ? content : "",
                        use_cases ? use_cases : "", confidence, session_id ? session_id : "", &out);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to store memory");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", (double)out.id);
   cJSON *obj = kbs_memory_row_to_json(&out);
   if (obj)
      cJSON_AddItemToObject(resp, "memory", obj);
   return resp;
}

cJSON *db2_kb_service_memory_briefing_json(int limit_tokens)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   cJSON *briefing = memory_briefing(limit_tokens);
   if (!briefing)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory_briefing failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "briefing", briefing);
   return resp;
}

cJSON *db2_kb_service_memory_context_block_json(const char *query, const char *block_type,
                                                int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   char *block = memory_get_context_block(query ? query : "",
                                          (block_type && block_type[0]) ? block_type : "general",
                                          limit > 0 ? limit : 5);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "block", block ? block : "");
   free(block);
   return resp;
}

static cJSON *kbs_memory_relation_to_json(const memory_relation_t *r)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)r->id);
   cJSON_AddNumberToObject(obj, "memory_id", (double)r->memory_id);
   cJSON_AddNumberToObject(obj, "episode_id", (double)r->episode_id);
   cJSON_AddStringToObject(obj, "src_entity", r->src_entity);
   cJSON_AddStringToObject(obj, "relation", r->relation);
   cJSON_AddStringToObject(obj, "dst_entity", r->dst_entity);
   cJSON_AddStringToObject(obj, "fact_text", r->fact_text);
   cJSON_AddStringToObject(obj, "valid_at", r->valid_at);
   cJSON_AddStringToObject(obj, "invalid_at", r->invalid_at);
   cJSON_AddNumberToObject(obj, "weight", r->weight);
   cJSON_AddStringToObject(obj, "created_at", r->created_at);
   return obj;
}

cJSON *db2_kb_service_memory_entity_profile_json(const char *entity)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_entity_profile_t profile;
   if (memory_get_entity_profile(entity ? entity : "", &profile) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "entity profile not found");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *prof = cJSON_AddObjectToObject(resp, "profile");
   if (!prof)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(prof, "entity", profile.entity);
   cJSON_AddNumberToObject(prof, "mention_count", profile.mention_count);
   cJSON_AddNumberToObject(prof, "relation_count", profile.relation_count);
   cJSON_AddStringToObject(prof, "latest_episode", profile.latest_episode);
   cJSON_AddStringToObject(prof, "summary", profile.summary);
   return resp;
}

cJSON *db2_kb_service_memory_find_id_by_key_kind_json(const char *key, const char *kind)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int64_t id = db2_memory_find_id_by_key_kind(key ? key : "", kind ? kind : "");
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "id", (double)id);
   return resp;
}

cJSON *db2_kb_service_memory_search_facts_patterns_by_keyword_json(const char *keyword, int max)
{
   if (max < 1)
      max = 5;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "memories") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[64];
   int n = db2_memory_search_facts_patterns_by_keyword(keyword ? keyword : "", rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_supersede_json(int64_t old_id, const char *new_content,
                                            double confidence, const char *session_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   memory_t out;
   memset(&out, 0, sizeof(out));
   int rc = memory_supersede(old_id, new_content ? new_content : "", confidence,
                             session_id ? session_id : "", &out);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory_supersede failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_memory_row_to_json(&out);
   if (obj)
      cJSON_AddItemToObject(resp, "memory", obj);
   return resp;
}

cJSON *db2_kb_service_memory_fact_history_json(const char *key, int max)
{
   if (max < 1)
      max = 16;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "history") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[64];
   int n = memory_fact_history(key ? key : "", rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_list_session_scope_priority_json(int max)
{
   if (max < 1)
      max = 24;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "memories") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[64];
   int n = db2_memory_list_session_scope_priority(rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_list_low_effectiveness_json(double threshold, int limit)
{
   if (limit < 1)
      limit = 50;
   if (limit > 256)
      limit = 256;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "rows") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   db2_memory_low_eff_row_t rows[256];
   int n = db2_memory_list_low_effectiveness(threshold, limit, rows, limit);
   for (int i = 0; i < n; i++)
   {
      cJSON *m = cJSON_CreateObject();
      if (!m)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(m, "id", (double)rows[i].id);
      cJSON_AddStringToObject(m, "tier", rows[i].tier);
      cJSON_AddStringToObject(m, "kind", rows[i].kind);
      cJSON_AddStringToObject(m, "key", rows[i].key);
      cJSON_AddNumberToObject(m, "effectiveness", rows[i].effectiveness);
      cJSON_AddNumberToObject(m, "use_count", rows[i].use_count);
      cJSON_AddItemToArray(arr, m);
   }
   return resp;
}

cJSON *db2_kb_service_memory_list_unused_l2_json(int days, int max)
{
   if (max < 1)
      max = 64;
   if (max > 256)
      max = 256;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "rows") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   db2_memory_unused_l2_row_t rows[256];
   int n = db2_memory_list_unused_l2(days, rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *m = cJSON_CreateObject();
      if (!m)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(m, "id", (double)rows[i].id);
      cJSON_AddStringToObject(m, "key", rows[i].key);
      cJSON_AddStringToObject(m, "tier", rows[i].tier);
      cJSON_AddStringToObject(m, "kind", rows[i].kind);
      cJSON_AddNumberToObject(m, "confidence", rows[i].confidence);
      cJSON_AddItemToArray(arr, m);
   }
   return resp;
}

cJSON *db2_kb_service_memory_list_superseded_keys_json(int min_versions, int max)
{
   if (max < 1)
      max = 64;
   if (max > 256)
      max = 256;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "rows") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   db2_memory_superseded_row_t rows[256];
   int n = db2_memory_list_superseded_keys(min_versions, rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *m = cJSON_CreateObject();
      if (!m)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddStringToObject(m, "base_key", rows[i].base_key);
      cJSON_AddNumberToObject(m, "versions", rows[i].versions);
      cJSON_AddItemToArray(arr, m);
   }
   return resp;
}

cJSON *db2_kb_service_memory_set_artifact_json(int64_t memory_id, const char *artifact_type,
                                               const char *artifact_ref, const char *artifact_hash)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_memory_set_artifact(memory_id, artifact_type ? artifact_type : "",
                                    artifact_ref ? artifact_ref : "", artifact_hash);
   if (rc < 1)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory not found");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_memory_list_session_scope_priority_like_json(const char *pattern, int max)
{
   if (max < 1)
      max = 5;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "memories") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_t rows[64];
   int n = db2_memory_list_session_scope_priority_like(pattern ? pattern : "", rows, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_row_to_json(&rows[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_check_drift_json(int64_t task_id, const char *file_path,
                                              const char *command)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   drift_result_t r;
   memset(&r, 0, sizeof(r));
   int rc = memory_check_drift(task_id, file_path ? file_path : "", command ? command : "", &r);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "task not found");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "drifted", r.drifted);
   cJSON_AddNumberToObject(resp, "task_id", (double)r.task_id);
   cJSON_AddStringToObject(resp, "task_title", r.task_title);
   cJSON_AddStringToObject(resp, "message", r.message);
   return resp;
}

static cJSON *kbs_task_row_to_json(const aimee_task_t *t)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;
   cJSON_AddNumberToObject(obj, "id", (double)t->id);
   cJSON_AddNumberToObject(obj, "parent_id", (double)t->parent_id);
   cJSON_AddStringToObject(obj, "title", t->title);
   cJSON_AddStringToObject(obj, "state", t->state);
   cJSON_AddNumberToObject(obj, "confidence", t->confidence);
   cJSON_AddStringToObject(obj, "created_at", t->created_at);
   cJSON_AddStringToObject(obj, "updated_at", t->updated_at);
   cJSON_AddStringToObject(obj, "session_id", t->session_id);
   return obj;
}

cJSON *db2_kb_service_task_create_json(const char *title, const char *session_id, int64_t parent_id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   aimee_task_t task;
   memset(&task, 0, sizeof(task));
   if (db2_task_create(title ? title : "", session_id ? session_id : "", parent_id, &task) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to create task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *obj = kbs_task_row_to_json(&task);
   if (obj)
      cJSON_AddItemToObject(resp, "task", obj);
   return resp;
}

cJSON *db2_kb_service_task_update_state_json(int64_t id, const char *state)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_update_state(id, state ? state : "");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to update task state");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_delete_json(int64_t id)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_delete(id);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to delete task");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_add_edge_json(int64_t source, int64_t target, const char *relation)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   int rc = db2_task_add_edge(source, target, relation ? relation : "depends_on");
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to add task edge");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   return resp;
}

cJSON *db2_kb_service_task_get_edges_json(int64_t task_id, int max)
{
   if (max < 1)
      max = 16;
   if (max > 64)
      max = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "edges") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   task_edge_t edges[64];
   int n = db2_task_get_edges(task_id, edges, max);
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      if (!e)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(e, "id", (double)edges[i].id);
      cJSON_AddNumberToObject(e, "source_id", (double)edges[i].source_id);
      cJSON_AddNumberToObject(e, "target_id", (double)edges[i].target_id);
      cJSON_AddStringToObject(e, "relation", edges[i].relation);
      cJSON_AddItemToArray(arr, e);
   }
   return resp;
}

cJSON *db2_kb_service_task_list_json(const char *state, const char *session_id, int limit)
{
   if (limit < 1)
      limit = 16;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "tasks") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   aimee_task_t rows[64];
   int n = db2_task_list((state && state[0]) ? state : NULL,
                         (session_id && session_id[0]) ? session_id : NULL, limit, rows, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddNumberToObject(obj, "id", (double)rows[i].id);
      cJSON_AddNumberToObject(obj, "parent_id", (double)rows[i].parent_id);
      cJSON_AddStringToObject(obj, "title", rows[i].title);
      cJSON_AddStringToObject(obj, "state", rows[i].state);
      cJSON_AddNumberToObject(obj, "confidence", rows[i].confidence);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(obj, "updated_at", rows[i].updated_at);
      cJSON_AddStringToObject(obj, "session_id", rows[i].session_id);
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_entity_edges_json(const char *entity, int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "edges") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_relation_t rels[64];
   int n = memory_get_entity_edges(entity ? entity : "", limit, rels, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_relation_to_json(&rels[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_search_graph_json(const char *query, int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "relations") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_relation_t rels[64];
   int n = memory_search_graph(query ? query : "", limit, rels, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_relation_to_json(&rels[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_search_graph_as_of_json(const char *query, const char *as_of,
                                                     int limit)
{
   if (limit < 1)
      limit = 10;
   if (limit > 64)
      limit = 64;

   cJSON *resp = cJSON_CreateObject();
   cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "relations") : NULL;
   if (!resp || !arr)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddStringToObject(resp, "status", "ok");

   memory_relation_t rels[64];
   int n = memory_search_graph_as_of(query ? query : "", as_of ? as_of : "", limit, rels, 64);
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = kbs_memory_relation_to_json(&rels[i]);
      if (!obj)
      {
         cJSON_Delete(resp);
         return NULL;
      }
      cJSON_AddItemToArray(arr, obj);
   }
   return resp;
}

cJSON *db2_kb_service_memory_get_episode_json(const char *episode_key)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   memory_episode_t episode;
   if (memory_get_episode(episode_key ? episode_key : "", &episode) != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "episode not found");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *ep = cJSON_AddObjectToObject(resp, "episode");
   if (!ep)
   {
      cJSON_Delete(resp);
      return NULL;
   }
   cJSON_AddNumberToObject(ep, "id", (double)episode.id);
   cJSON_AddNumberToObject(ep, "memory_id", (double)episode.memory_id);
   cJSON_AddStringToObject(ep, "episode_key", episode.episode_key);
   cJSON_AddStringToObject(ep, "episode_text", episode.episode_text);
   cJSON_AddStringToObject(ep, "source_session", episode.source_session);
   cJSON_AddStringToObject(ep, "reference_time", episode.reference_time);
   cJSON_AddStringToObject(ep, "created_at", episode.created_at);
   return resp;
}

cJSON *db2_kb_service_memory_ask_json(const char *query, const char *scope_type,
                                      const char *scope_value, int limit)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;

   memory_answer_result_t result;
   memset(&result, 0, sizeof(result));
   int rc;
   if (scope_type && scope_type[0])
      rc = memory_ask_query_scoped(query ? query : "", scope_type, scope_value,
                                   limit > 0 ? limit : 5, &result);
   else
      rc = memory_ask_query(query ? query : "", limit > 0 ? limit : 5, &result);
   if (rc != 0)
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message",
                              result.error[0] ? result.error : "memory_ask failed");
      return resp;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "answer", result.answer);
   cJSON_AddNumberToObject(resp, "confidence", result.confidence);
   cJSON_AddBoolToObject(resp, "no_answer", result.no_answer);
   cJSON_AddBoolToObject(resp, "low_confidence", result.low_confidence);
   cJSON_AddNumberToObject(resp, "retrieval_count", result.retrieval_count);
   cJSON *citations = cJSON_AddArrayToObject(resp, "citation_ids");
   for (int i = 0; i < result.citation_count; i++)
      cJSON_AddItemToArray(citations, cJSON_CreateNumber((double)result.citation_ids[i]));
   return resp;
}

cJSON *db2_kb_service_memory_lint_json(void)
{
   memory_lint_issue_t issues[MEMORY_LINT_MAX_ISSUES];
   int n = memory_lint_run(issues, MEMORY_LINT_MAX_ISSUES);
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "issue_count", n);
   cJSON *arr = cJSON_AddArrayToObject(resp, "issues");
   for (int i = 0; i < n; i++)
   {
      cJSON *iss = cJSON_CreateObject();
      if (!iss)
         break;
      cJSON_AddStringToObject(iss, "type", issues[i].type);
      if (issues[i].memory_id)
         cJSON_AddNumberToObject(iss, "memory_id", (double)issues[i].memory_id);
      cJSON_AddStringToObject(iss, "key", issues[i].key);
      cJSON_AddStringToObject(iss, "message", issues[i].message);
      cJSON_AddItemToArray(arr, iss);
   }
   return resp;
}
