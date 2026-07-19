/* cmd_learning.c: learning proposal review and operator commits. */
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"
#include "kb_client.h"

static void learning_emit_list(app_ctx_t *ctx, learning_proposal_t *rows, int count)
{
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *item = learning_proposal_to_json(&rows[i]);
         if (item)
            cJSON_AddItemToArray(arr, item);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      return;
   }

   if (count == 0)
   {
      printf("No learning proposals.\n");
      return;
   }

   for (int i = 0; i < count; i++)
   {
      printf("#%d %s %s", rows[i].id, rows[i].sink, rows[i].state);
      if (rows[i].target_key[0])
         printf(" target=%s", rows[i].target_key);
      if (rows[i].target_memory_id > 0)
         printf(" memory=%lld", (long long)rows[i].target_memory_id);
      printf(" corroboration=%d\n", rows[i].corroboration_count);
      if (rows[i].expires_at[0])
         printf("  expires_at: %s\n", rows[i].expires_at);
      if (rows[i].archive_reason[0])
         printf("  archive_reason: %s\n", rows[i].archive_reason);
      printf("  action: %s\n", rows[i].action_json);
      if (rows[i].evidence_refs[0] && strcmp(rows[i].evidence_refs, "[]") != 0)
         printf("  evidence_refs: %s\n", rows[i].evidence_refs);
   }
}

static void learning_emit_metrics(app_ctx_t *ctx, int window_days)
{
   (void)ctx;
   learning_commit_ratio_t cr = {0};
   learning_sink_cap_t caps[LEARNING_SINK_COUNT] = {{{0}, 0, 0, 0.0}};
   learning_metrics_commit_ratio(window_days, &cr);
   int cap_rows = learning_metrics_per_sink_caps(caps, LEARNING_SINK_COUNT);

   int64_t ingest_calls = 0;
   double ingest_ms_avg = 0.0, ingest_ms_max = 0.0;
   learning_router_metrics(&ingest_calls, &ingest_ms_avg, &ingest_ms_max);

   int64_t detect_calls = 0;
   double detect_ms_avg = 0.0, detect_ms_max = 0.0;
   learning_router_detection_metrics(&detect_calls, &detect_ms_avg, &detect_ms_max);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON *commit = cJSON_AddObjectToObject(obj, "commit_ratio");
      cJSON_AddNumberToObject(commit, "window_days", cr.window_days);
      cJSON_AddNumberToObject(commit, "committed", (double)cr.proposals_committed);
      cJSON_AddNumberToObject(commit, "terminal", (double)cr.proposals_terminal);
      cJSON_AddNumberToObject(commit, "ratio", cr.commit_ratio);
      cJSON *sinks = cJSON_AddArrayToObject(obj, "per_sink_caps");
      for (int i = 0; i < cap_rows; i++)
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddStringToObject(row, "sink", caps[i].sink);
         cJSON_AddNumberToObject(row, "committed_this_week", caps[i].committed_this_week);
         cJSON_AddNumberToObject(row, "weekly_cap", caps[i].weekly_cap);
         cJSON_AddNumberToObject(row, "utilization", caps[i].utilization);
         cJSON_AddItemToArray(sinks, row);
      }
      cJSON *ingest = cJSON_AddObjectToObject(obj, "ingest_latency_ms");
      cJSON_AddNumberToObject(ingest, "calls", (double)ingest_calls);
      cJSON_AddNumberToObject(ingest, "avg", ingest_ms_avg);
      cJSON_AddNumberToObject(ingest, "max", ingest_ms_max);
      cJSON *detect = cJSON_AddObjectToObject(obj, "detection_latency_ms");
      cJSON_AddNumberToObject(detect, "calls", (double)detect_calls);
      cJSON_AddNumberToObject(detect, "avg", detect_ms_avg);
      cJSON_AddNumberToObject(detect, "max", detect_ms_max);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("# Learning Metrics\n\n");
   printf("commit_ratio (last %d days): %.3f (%lld committed / %lld settled)\n", cr.window_days,
          cr.commit_ratio, (long long)cr.proposals_committed, (long long)cr.proposals_terminal);
   printf("\nper-sink weekly caps:\n");
   for (int i = 0; i < cap_rows; i++)
   {
      printf("  %-10s %d/%d (%.0f%%)\n", caps[i].sink, caps[i].committed_this_week,
             caps[i].weekly_cap, caps[i].utilization * 100.0);
   }
   printf("\nsignal-ingest latency:  calls=%lld avg=%.2fms max=%.2fms\n", (long long)ingest_calls,
          ingest_ms_avg, ingest_ms_max);
   printf("detection latency:      calls=%lld avg=%.2fms max=%.2fms\n", (long long)detect_calls,
          detect_ms_avg, detect_ms_max);
}

/* Parse a JSON envelope returned by kb_client_learning_*; fatal on error.
 * Returns the owned cJSON* the caller must cJSON_Delete. */
static cJSON *learning_rpc_unwrap(char *resp_json, const char *what)
{
   cJSON *resp = resp_json ? cJSON_Parse(resp_json) : NULL;
   free(resp_json);
   cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
   {
      const char *msg = what;
      if (resp)
      {
         cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(m) && m->valuestring[0])
            msg = m->valuestring;
      }
      char buf[256];
      snprintf(buf, sizeof(buf), "%s", msg);
      cJSON_Delete(resp);
      fatal("%s", buf);
   }
   return resp;
}

static void cmd_learning_list(app_ctx_t *ctx, int argc, char **argv)
{
   const char *state = argc >= 2 ? argv[1] : "pending";
   int limit = argc >= 3 ? atoi(argv[2]) : 50;
   if (limit < 1)
      limit = 1;
   if (limit > 64)
      limit = 64;
   cJSON *resp = learning_rpc_unwrap(kb_client_learning_list_proposals_json(state, NULL, limit),
                                     "failed to list learning proposals");
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "proposals");
   int count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
   learning_proposal_t rows[64];
   int n = 0;
   for (int i = 0; i < count && n < 64; i++)
   {
      cJSON *item = cJSON_GetArrayItem(arr, i);
      if (learning_proposal_from_json(item, &rows[n]) == 0)
         n++;
   }
   cJSON_Delete(resp);
   learning_emit_list(ctx, rows, n);
   return;
}

static void learning_proposal_action(app_ctx_t *ctx, int argc, char **argv, const char *sub)
{
   if (argc < 2)
      fatal("learning %s requires a proposal id", sub);
   int id = atoi(argv[1]);
   char *(*rpc)(int) = strcmp(sub, "status") == 0
                           ? kb_client_learning_get_proposal_json
                           : (strcmp(sub, "accept") == 0 ? kb_client_learning_accept_proposal_json
                                                         : kb_client_learning_reject_proposal_json);
   char err[64];
   snprintf(err, sizeof(err), "learning proposal %s failed", sub);
   cJSON *resp = learning_rpc_unwrap(rpc(id), err);
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "proposal");
   learning_proposal_t proposal;
   if (!p || learning_proposal_from_json(p, &proposal) != 0)
   {
      cJSON_Delete(resp);
      fatal("learning proposal %s: unexpected response", sub);
   }
   cJSON_Delete(resp);

   if (strcmp(sub, "status") == 0)
   {
      learning_emit_list(ctx, &proposal, 1);
   }
   else if (ctx->json_output)
   {
      cJSON *obj = learning_proposal_to_json(&proposal);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("learning proposal #%d is %s\n", proposal.id, proposal.state);
   }
   return;
}

static void cmd_learning_metrics(app_ctx_t *ctx, int argc, char **argv)
{
   int window_days = argc >= 2 ? atoi(argv[1]) : 0;
   learning_emit_metrics(ctx, window_days);
   return;
}

static void cmd_learning_status(app_ctx_t *ctx, int argc, char **argv)
{
   learning_proposal_action(ctx, argc, argv, "status");
}

static void cmd_learning_accept(app_ctx_t *ctx, int argc, char **argv)
{
   learning_proposal_action(ctx, argc, argv, "accept");
}

static void cmd_learning_reject(app_ctx_t *ctx, int argc, char **argv)
{
   learning_proposal_action(ctx, argc, argv, "reject");
}

static const subcmd_t cmd_learning_subs[] = {
    {"list", "list learning proposals", cmd_learning_list},
    {"status", "show a proposal's status", cmd_learning_status},
    {"accept", "accept a proposal", cmd_learning_accept},
    {"reject", "reject a proposal", cmd_learning_reject},
    {"metrics", "show learning metrics", cmd_learning_metrics},
    {NULL, NULL, NULL},
};

void cmd_learning(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("learning requires a subcommand: list, status, accept, reject, metrics");

   const char *sub = argv[0];

   if (subcmd_dispatch(cmd_learning_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown learning subcommand: %s", sub);
}
