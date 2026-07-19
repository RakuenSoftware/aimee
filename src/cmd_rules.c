/* cmd_rules.c: rules and feedback CLI (list, generate, delete, +/- with directive flags) */
#include "aimee.h"
#include "cmd_review.h"
#include "log.h"
#include "commands.h"
#include "kb_client.h"
#include "cJSON.h"

/* --- cmd_rules --- */

static void cmd_rules_list(app_ctx_t *ctx, int argc, char **argv)
{
   char *json = kb_client_rules_list_json(256);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   cJSON *arr = resp ? cJSON_GetObjectItemCaseSensitive(resp, "rules") : NULL;
   if (ctx->json_output)
   {
      cJSON *out = arr ? cJSON_DetachItemViaPointer(resp, arr) : cJSON_CreateArray();
      emit_json_ctx(out, ctx->json_fields, ctx->response_profile);
   }
   cJSON_Delete(resp);
}

static void cmd_rules_generate(app_ctx_t *ctx, int argc, char **argv)
{
   char *json = kb_client_rules_generate_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   cJSON *content = resp ? cJSON_GetObjectItemCaseSensitive(resp, "content") : NULL;
   const char *md = cJSON_IsString(content) ? content->valuestring : "";
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "content", md);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else if (md[0])
   {
      printf("%s", md);
   }
   cJSON_Delete(resp);
}

static void cmd_rules_delete(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("rules delete requires an id");
   int id = atoi(argv[0]);
   kb_client_rules_delete(id);
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void cmd_rules_review(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_review_surface("rule", ctx, argc, argv);
}

static const subcmd_t cmd_rules_subs[] = {
    {"list", "list rules", cmd_rules_list},
    {"generate", "generate a rules digest", cmd_rules_generate},
    {"delete", "delete a rule by id", cmd_rules_delete},
    {"review", "review surfaced rules", cmd_rules_review},
    {NULL, NULL, NULL},
};

void cmd_rules(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("rules requires a subcommand: list, generate, delete");

   const char *sub = argv[0];
   argc--;
   argv++;

   if (subcmd_dispatch(cmd_rules_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown rules subcommand: %s", sub);
}

/* --- cmd_feedback --- */

static void cmd_feedback_impl(app_ctx_t *ctx, int argc, char **argv, const char *polarity_hint)
{
   static const char *bool_flags[] = {"hard", "soft", "session", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   int weight = opt_get_int(&opts, "weight", -1);
   const char *directive_type = NULL;
   if (opt_has(&opts, "hard"))
      directive_type = "hard";
   else if (opt_has(&opts, "soft"))
      directive_type = "soft";
   else if (opt_has(&opts, "session"))
      directive_type = "session";
   const char *polarity_str = polarity_hint ? polarity_hint : opt_pos(&opts, 0);
   const char *description = polarity_hint ? opt_pos(&opts, 0) : opt_pos(&opts, 1);

   if (!polarity_str)
      fatal("feedback requires a polarity (+, -, principle)");
   if (!description)
      fatal("feedback requires a description");

   const char *polarity = db2_feedback_parse_polarity(polarity_str);
   if (!polarity)
      fatal("invalid polarity: %s", polarity_str);

   if (learning_router_enabled())
   {
      const char *signal_type;
      if (strcmp(polarity, "positive") == 0)
         signal_type = "thumb_up";
      else if (strcmp(polarity, "negative") == 0)
         signal_type = "thumb_down";
      else
         signal_type = directive_type ? "mark_rule" : "preference_statement";

      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "signal_type", signal_type);
      cJSON_AddStringToObject(req, "source", "explicit");
      cJSON_AddStringToObject(req, "polarity", polarity);
      cJSON_AddStringToObject(req, "title", description);
      cJSON_AddStringToObject(req, "description", description);
      if (directive_type && strcmp(directive_type, "hard") == 0)
         cJSON_AddBoolToObject(req, "high_confidence", 1);

      char *envelope = kb_client_learning_propose_signal_json(req);
      cJSON_Delete(req);
      cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
      free(envelope);
      cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
      {
         cJSON_Delete(resp);
         fatal("failed to route feedback through learning subsystem");
      }

      if (ctx->json_output)
      {
         cJSON *dispatch = cJSON_GetObjectItemCaseSensitive(resp, "dispatch");
         cJSON *out = dispatch ? cJSON_DetachItemViaPointer(resp, dispatch) : cJSON_CreateObject();
         emit_json_ctx(out, ctx->json_fields, ctx->response_profile);
      }

      cJSON_Delete(resp);
      return;
   }

   int reinforced = 0;
   int id = kb_client_feedback_record(polarity, description, "", weight, &reinforced);
   if (id < 0)
      fatal("failed to record feedback");

   /* Set directive type if specified (Feature 15) */
   if (directive_type && id > 0)
   {
      if (kb_client_rules_update_directive_type(id, directive_type) != 0)
         LOG_ERROR("cmd_feedback_impl", "failed to update directive type for rule %d", id);
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddNumberToObject(j, "id", id);
      cJSON_AddBoolToObject(j, "reinforced", reinforced);
      if (directive_type)
         cJSON_AddStringToObject(j, "directive_type", directive_type);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
}

void cmd_feedback_raw(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_feedback_impl(ctx, argc, argv, NULL);
}

void cmd_feedback_plus(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_feedback_impl(ctx, argc, argv, "+");
}

void cmd_feedback_minus(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_feedback_impl(ctx, argc, argv, "-");
}
