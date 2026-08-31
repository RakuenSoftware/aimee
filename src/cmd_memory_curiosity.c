/* cmd_memory_curiosity.c: CLI surface for the curiosity backlog.
 *
 *   aimee memory curiosity list [--state open|in_progress|resolved|suppressed]
 *                               [--limit N] [--json]
 *   aimee memory curiosity create --gap-type X --target-topic T \
 *                                 [--target-entity E] [--evidence S]
 *                                 [--importance 0..1] [--novelty 0..1]
 *   aimee memory curiosity sweep          (populate missing_fact items
 *                                          from failed_queries)
 *   aimee memory curiosity suppress <id>
 *   aimee memory curiosity resolve <id>
 *
 * Completed phase-3 operator surface. See
 * docs/proposals/done/personal-agent-phase-3-curiosity.md. */

#include "aimee.h"
#include "cmd_memory_internal.h"
#include "commands.h"
#include "modules/db2/c/curiosity.h"
#include "kb_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sub_list(app_ctx_t *ctx, int argc, char **argv)
{
   const char *state = NULL;
   int limit = 50;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--state") == 0 && i + 1 < argc)
      {
         state = argv[++i];
         if (!db2_curiosity_state_is_valid(state))
            fatal("--state must be one of open/in_progress/resolved/suppressed");
      }
      else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
         limit = atoi(argv[++i]);
      else
         fatal("unknown curiosity list flag: %s", argv[i]);
   }
   if (limit <= 0)
      limit = 50;
   if (limit > 256)
      limit = 256;

   /* Shared knowledge is owned by the knowledge service. Reach the curiosity
    * backlog through RPC rather than opening storage locally. */
   char *json = kb_client_curiosity_list_json(state, limit);
   if (!json)
      fatal("knowledge service unavailable");
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      fatal("invalid response from knowledge service");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      cJSON_Delete(resp);
      fatal("knowledge service error: %s", cJSON_IsString(msg) ? msg->valuestring : "unknown");
   }
   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "items");
   int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;

   if (ctx->json_output)
   {
      /* Forward the items array directly (already in curiosity_item_t shape). */
      cJSON *arr = items ? cJSON_DetachItemViaPointer(resp, items) : cJSON_CreateArray();
      cJSON_Delete(resp);
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      return;
   }
   if (n == 0)
   {
      printf("No curiosity items%s%s.\n", state ? " in state " : "", state ? state : "");
      cJSON_Delete(resp);
      return;
   }
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, items)
   {
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(it, "id");
      cJSON *gap_j = cJSON_GetObjectItemCaseSensitive(it, "gap_type");
      cJSON *state_j = cJSON_GetObjectItemCaseSensitive(it, "state");
      cJSON *entity_j = cJSON_GetObjectItemCaseSensitive(it, "target_entity");
      cJSON *topic_j = cJSON_GetObjectItemCaseSensitive(it, "target_topic");
      cJSON *evidence_j = cJSON_GetObjectItemCaseSensitive(it, "evidence");
      const char *entity =
          cJSON_IsString(entity_j) && entity_j->valuestring[0] ? entity_j->valuestring : "-";
      const char *topic =
          cJSON_IsString(topic_j) && topic_j->valuestring[0] ? topic_j->valuestring : "-";
      printf("[%lld] %-24s  %-10s  %s / %s\n",
             cJSON_IsNumber(id_j) ? (long long)id_j->valuedouble : 0LL,
             cJSON_IsString(gap_j) ? gap_j->valuestring : "",
             cJSON_IsString(state_j) ? state_j->valuestring : "", entity, topic);
      if (cJSON_IsString(evidence_j) && evidence_j->valuestring[0])
         printf("    %s\n", evidence_j->valuestring);
   }
   cJSON_Delete(resp);
}

static void sub_create(app_ctx_t *ctx, int argc, char **argv)
{
   const char *gap_type = NULL;
   const char *target_entity = NULL;
   const char *target_topic = NULL;
   const char *evidence = NULL;
   double importance = 0.0;
   double novelty = 0.0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--gap-type") == 0 && i + 1 < argc)
         gap_type = argv[++i];
      else if (strcmp(argv[i], "--target-entity") == 0 && i + 1 < argc)
         target_entity = argv[++i];
      else if (strcmp(argv[i], "--target-topic") == 0 && i + 1 < argc)
         target_topic = argv[++i];
      else if (strcmp(argv[i], "--evidence") == 0 && i + 1 < argc)
         evidence = argv[++i];
      else if (strcmp(argv[i], "--importance") == 0 && i + 1 < argc)
         importance = atof(argv[++i]);
      else if (strcmp(argv[i], "--novelty") == 0 && i + 1 < argc)
         novelty = atof(argv[++i]);
      else
         fatal("unknown curiosity create flag: %s", argv[i]);
   }
   if (!gap_type || !gap_type[0])
      fatal("curiosity create: --gap-type is required "
            "(missing_fact|contradiction|stale_fact|weak_coverage|unverified_assumption)");
   if (!db2_curiosity_gap_type_is_canonical(gap_type))
      fatal("curiosity create: unknown gap_type \"%s\"", gap_type);
   if ((!target_entity || !target_entity[0]) && (!target_topic || !target_topic[0]))
      fatal("curiosity create: one of --target-entity or --target-topic is required");

   /* Route the write through the knowledge service RPC. */
   char *json = kb_client_curiosity_create_json(gap_type, target_entity, target_topic, evidence,
                                                importance, novelty, session_id());
   if (!json)
      fatal("knowledge service unavailable");
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      fatal("invalid response from knowledge service");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      cJSON_Delete(resp);
      fatal("curiosity create: %s", cJSON_IsString(msg) ? msg->valuestring : "write failed");
   }
   cJSON *item = cJSON_GetObjectItemCaseSensitive(resp, "item");
   if (ctx->json_output)
   {
      cJSON *detached = item ? cJSON_DetachItemViaPointer(resp, item) : cJSON_CreateObject();
      cJSON_Delete(resp);
      emit_json_ctx(detached, ctx->json_fields, ctx->response_profile);
      return;
   }
   cJSON *id_j = item ? cJSON_GetObjectItemCaseSensitive(item, "id") : NULL;
   cJSON *gap_j = item ? cJSON_GetObjectItemCaseSensitive(item, "gap_type") : NULL;
   printf("created curiosity item %lld (%s)\n",
          cJSON_IsNumber(id_j) ? (long long)id_j->valuedouble : 0LL,
          cJSON_IsString(gap_j) ? gap_j->valuestring : "");
   cJSON_Delete(resp);
}

static int rpc_simple_count(const char *json, const char *field, const char *err_label)
{
   if (!json)
      fatal("knowledge service unavailable for %s", err_label);
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
      fatal("invalid response from knowledge service (%s)", err_label);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      char buf[256];
      snprintf(buf, sizeof(buf), "%s: %s", err_label,
               cJSON_IsString(msg) ? msg->valuestring : "failed");
      cJSON_Delete(resp);
      fatal("%s", buf);
   }
   cJSON *count_j = cJSON_GetObjectItemCaseSensitive(resp, field);
   int count = cJSON_IsNumber(count_j) ? (int)count_j->valuedouble : 0;
   cJSON_Delete(resp);
   return count;
}

static void sub_sweep(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argv;
   if (argc > 0)
      fatal("curiosity sweep takes no arguments");
   char *json = kb_client_curiosity_sweep_json();
   int created = rpc_simple_count(json, "created", "curiosity sweep");
   free(json);
   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "created", created);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
      printf("swept failed_queries; created %d new curiosity item(s)\n", created);
}

static void sub_rescore(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argv;
   if (argc > 0)
      fatal("curiosity rescore takes no arguments");
   char *json = kb_client_curiosity_rescore_json();
   int rescored = rpc_simple_count(json, "rescored", "curiosity rescore");
   free(json);
   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "rescored", rescored);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
      printf("rescored %d curiosity item(s)\n", rescored);
}

static void sub_route(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   int limit = 5;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
         limit = atoi(argv[++i]);
      else
         fatal("unknown curiosity route flag: %s", argv[i]);
   }
   if (limit <= 0)
      limit = 5;
   if (limit > 32)
      limit = 32;
   /* Rescore + route_top run as a single knowledge-service RPC so the
    * top-N decision and the directive insert all execute under the
    * same service transaction. */
   {
      char *json = kb_client_curiosity_rescore_json();
      free(json); /* discard count; route_top RPC re-reads scores */
   }
   char *route_json = kb_client_curiosity_route_top_json(limit, session_id());
   int routed = rpc_simple_count(route_json, "routed", "curiosity route");
   free(route_json);
   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "routed", routed);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
      printf("routed %d curiosity item(s) to directives\n", routed);
}

static void sub_transition(app_ctx_t *ctx, int argc, char **argv, const char *to)
{
   if (argc < 1)
      fatal("curiosity %s requires <id>", to);
   int64_t id = (int64_t)atoll(argv[0]);
   if (id <= 0)
      fatal("curiosity %s: invalid id", to);

   /* Fetch the row via RPC for the "from -> to" message. */
   char *get_json = kb_client_curiosity_get_json(id);
   if (!get_json)
      fatal("knowledge service unavailable");
   cJSON *get_resp = cJSON_Parse(get_json);
   free(get_json);
   if (!get_resp)
      fatal("invalid response from knowledge service");
   cJSON *get_status = cJSON_GetObjectItemCaseSensitive(get_resp, "status");
   if (!cJSON_IsString(get_status) || strcmp(get_status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(get_resp, "message");
      char buf[256];
      snprintf(buf, sizeof(buf), "curiosity %s: %s", to,
               cJSON_IsString(msg) ? msg->valuestring : "lookup failed");
      cJSON_Delete(get_resp);
      fatal("%s", buf);
   }
   cJSON *before_item = cJSON_GetObjectItemCaseSensitive(get_resp, "item");
   cJSON *before_state_j =
       before_item ? cJSON_GetObjectItemCaseSensitive(before_item, "state") : NULL;
   char from_state[32] = "";
   if (cJSON_IsString(before_state_j))
      snprintf(from_state, sizeof(from_state), "%s", before_state_j->valuestring);
   cJSON_Delete(get_resp);

   char *upd_json = kb_client_curiosity_update_state_json(id, to);
   if (!upd_json)
      fatal("knowledge service unavailable");
   cJSON *upd_resp = cJSON_Parse(upd_json);
   free(upd_json);
   if (!upd_resp)
      fatal("invalid response from knowledge service");
   cJSON *upd_status = cJSON_GetObjectItemCaseSensitive(upd_resp, "status");
   if (!cJSON_IsString(upd_status) || strcmp(upd_status->valuestring, "ok") != 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(upd_resp, "message");
      char buf[256];
      snprintf(buf, sizeof(buf), "curiosity %s: %s", to,
               cJSON_IsString(msg) ? msg->valuestring : "update failed");
      cJSON_Delete(upd_resp);
      fatal("%s", buf);
   }
   if (ctx->json_output)
   {
      cJSON *after = cJSON_GetObjectItemCaseSensitive(upd_resp, "item");
      cJSON *detached = after ? cJSON_DetachItemViaPointer(upd_resp, after) : cJSON_CreateObject();
      cJSON_Delete(upd_resp);
      emit_json_ctx(detached, ctx->json_fields, ctx->response_profile);
      return;
   }
   printf("curiosity item %lld: %s -> %s\n", (long long)id, from_state, to);
   cJSON_Delete(upd_resp);
}

static void sub_suppress(app_ctx_t *ctx, int argc, char **argv)
{
   sub_transition(ctx, argc, argv, CURIOSITY_STATE_SUPPRESSED);
}

static void sub_resolve(app_ctx_t *ctx, int argc, char **argv)
{
   sub_transition(ctx, argc, argv, CURIOSITY_STATE_RESOLVED);
}

static const subcmd_t mem_curiosity_subs[] = {
    {"list", "list curiosities", sub_list},
    {"create", "create a curiosity", sub_create},
    {"sweep", "sweep curiosities", sub_sweep},
    {"rescore", "rescore curiosities", sub_rescore},
    {"route", "route curiosities", sub_route},
    {"suppress", "suppress a curiosity", sub_suppress},
    {"resolve", "resolve a curiosity", sub_resolve},
    {NULL, NULL, NULL},
};

void mem_curiosity(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("curiosity requires a subcommand: list, create, sweep, suppress, resolve");
   const char *sub = argv[0];
   argc--;
   argv++;
   if (subcmd_dispatch(mem_curiosity_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown curiosity subcommand: %s", sub);
}
