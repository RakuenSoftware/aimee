#include "module_commands.h"
#include "json_fluent.h"
/* kb/db2_adapters/kb_service_backend_context.c: temporal semantic recall and default-on
 * typed context assembly. Kept separate from the compatibility memory RPCs so
 * the context contract can evolve without growing their translation unit. */

#include "kb_service_backend.h"

#include "aimee.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "db2_learning.h"
#include "lifecycle.h"
#include "memory_scope_query.h"
#include "modules/memory/memory_bus_context.h"
#include <errno.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Transitional transport for the Go owner's complete assertion result. */
cJSON *db2_kb_service_memory_search_assertions_json(const char *query, const char *valid_at,
                                                    const char *believed_at, int include_historical,
                                                    int max_hops, int limit)
{
   cJSON *request = cJSON_CreateObject(), *response = NULL;
   if (!request || !cJSON_AddStringToObject(request, "operation", "assertion-search") ||
       !cJSON_AddStringToObject(request, "query", query ? query : "") ||
       !cJSON_AddStringToObject(request, "valid_at", valid_at ? valid_at : "") ||
       !cJSON_AddStringToObject(request, "believed_at", believed_at ? believed_at : "") ||
       !cJSON_AddBoolToObject(request, "include_historical", include_historical) ||
       !cJSON_AddNumberToObject(request, "max_hops", max_hops) ||
       !cJSON_AddNumberToObject(request, "limit", limit) || memory_bus_add_context(request) != 0)
   {
      cJSON_Delete(request);
      return NULL;
   }
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &response);
   cJSON_Delete(request);
   if (rc <= 0 || !cJSON_IsObject(response))
   {
      cJSON_Delete(response);
      return NULL;
   }
   cJSON *assertions = cJSON_GetObjectItemCaseSensitive(response, "assertions");
   if (!cJSON_IsArray(assertions) || cJSON_GetArraySize(assertions) > 64)
   {
      cJSON_Delete(response);
      return NULL;
   }
   cJSON *item;
   cJSON_ArrayForEach(item, assertions)
   {
      const char *id = jo_cstr(item, "stable_id");
      char *end = NULL;
      errno = 0;
      long long parsed = strtoll(id, &end, 10);
      char canonical[32];
      snprintf(canonical, sizeof(canonical), "%lld", parsed);
      if (errno || !end || *end || parsed <= 0 || strcmp(id, canonical) ||
          !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "rendered")) ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(item, "historical")))
      {
         cJSON_Delete(response);
         return NULL;
      }
      /* cJSON parses numbers as doubles. Reinstall the Go owner's exact token
       * before this transitional response is serialized or copied again. */
      cJSON *exact = cJSON_CreateRaw(id);
      if (!exact || !cJSON_ReplaceItemInObjectCaseSensitive(item, "assertion_id", exact))
      {
         cJSON_Delete(exact);
         cJSON_Delete(response);
         return NULL;
      }
   }
   return response;
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
      int hops = 0;
      const cJSON *hops_j = cJSON_GetObjectItemCaseSensitive(req, "max_hops");
      if (cJSON_IsNumber(hops_j))
         hops = (int)hops_j->valuedouble;
      if (hops < 0)
         hops = 0;
      if (hops > 2)
         hops = 2;
      cJSON *semantic = db2_kb_service_memory_search_assertions_json(query, valid_at, believed_at,
                                                                     historical_enabled, hops, 32);
      cJSON *hits = cJSON_GetObjectItemCaseSensitive(semantic, "assertions");
      int vector_available = strcmp(jo_cstr(semantic, "channel_status"), "ok") == 0;
      if (strcmp(jo_cstr(semantic, "error_type"), "invalid_timestamp") == 0)
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
         cJSON_Delete(semantic);
         return resp;
      }
      if (!semantic || !cJSON_IsArray(hits) || strcmp(jo_cstr(semantic, "status"), "degraded") == 0)
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
         cJSON *hit;
         cJSON_ArrayForEach(hit, hits)
         {
            cJSON *item = cJSON_Duplicate(hit, 1);
            const char *rendered = jo_cstr(hit, "rendered");
            const char *stable_id = jo_cstr(hit, "stable_id");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(hit, "historical")))
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
      cJSON_Delete(semantic);
   }

   db2_memory_scope_context_t scope;
   db2_memory_scope_context_get(&scope);
   if (episodes_enabled || summaries_enabled)
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddBoolToObject(request, "scope_context", scope.active);
      cJSON_AddBoolToObject(request, "include_all", scope.include_all);
      cJSON_AddStringToObject(request, "workspace", scope.workspace);
      cJSON_AddStringToObject(request, "project", scope.project);
      cJSON_AddStringToObject(request, "scope_type", scope.scope_type);
      cJSON_AddStringToObject(request, "scope_value", scope.scope_value);
      cJSON_AddStringToObject(request, "query", query);
      cJSON_AddStringToObject(request, "entity", query);
      cJSON_AddNumberToObject(request, "limit", 16);
      if (episodes_enabled)
      {
         cJSON_AddStringToObject(request, "operation", "episode-list");
         cJSON *response = NULL;
         int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &response);
         cJSON *episodes = cJSON_GetObjectItemCaseSensitive(response, "episodes");
         if (rc <= 0 || !cJSON_IsArray(episodes))
         {
            degraded = 1;
            cJSON_ReplaceItemInObjectCaseSensitive(episodes_ch, "status",
                                                   cJSON_CreateString("degraded"));
            cJSON_AddStringToObject(episodes_ch, "reason", "episode retrieval unavailable");
         }
         cJSON *episode;
         cJSON_ArrayForEach(episode, episodes)
         {
            cJSON *item = cJSON_CreateObject();
            char stable_id[64];
            snprintf(stable_id, sizeof(stable_id), "%.0f", jo_num(episode, "id", 0));
            cJSON_AddStringToObject(item, "stable_id", stable_id);
            cJSON_AddStringToObject(item, "episode_key", jo_cstr(episode, "episode_key"));
            cJSON_AddStringToObject(item, "excerpt", jo_cstr(episode, "episode_text"));
            cJSON_AddStringToObject(item, "source_session", jo_cstr(episode, "source_session"));
            cJSON_AddStringToObject(item, "reference_time", jo_cstr(episode, "reference_time"));
            cJSON_AddStringToObject(item, "trust", "untrusted_data");
            included_count += kbs_channel_try_add(episodes_ch, trace, "episodes", stable_id, item,
                                                  jo_cstr(episode, "episode_text"), &episodes_used,
                                                  episodes_budget, &total_used, total_budget);
         }
         cJSON_Delete(response);
         cJSON_DeleteItemFromObjectCaseSensitive(request, "operation");
      }
      if (summaries_enabled && query[0])
      {
         cJSON_AddStringToObject(request, "operation", "entity-profile");
         cJSON *response = NULL;
         int rc = aimee_module_commands_dispatch_internal("memory.runtime", request, &response);
         cJSON *profile = cJSON_GetObjectItemCaseSensitive(response, "profile");
         if (rc <= 0 ||
             (!cJSON_IsObject(profile) && strcmp(jo_cstr(response, "kind"), "not_found")))
         {
            degraded = 1;
            cJSON_ReplaceItemInObjectCaseSensitive(summaries_ch, "status",
                                                   cJSON_CreateString("degraded"));
            cJSON_AddStringToObject(summaries_ch, "reason", "entity summary unavailable");
         }
         if (cJSON_IsObject(profile) && jo_cstr(profile, "summary")[0])
         {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "entity", jo_cstr(profile, "entity"));
            cJSON_AddStringToObject(item, "summary", jo_cstr(profile, "summary"));
            cJSON_AddStringToObject(item, "authority", "derived_noncanonical");
            included_count +=
                kbs_channel_try_add(summaries_ch, trace, "summaries", jo_cstr(profile, "entity"),
                                    item, jo_cstr(profile, "summary"), &summaries_used,
                                    summaries_budget, &total_used, total_budget);
         }
         cJSON_Delete(response);
      }
      cJSON_Delete(request);
   }

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
