/* cmd_wm.c: working memory CLI (session-scoped key-value scratch space) */
#include "aimee.h"
#include "commands.h"
#include "db1_client/db1.h"
#include "cJSON.h"

static const char *resolve_session(const opt_parsed_t *opts)
{
   const char *s = opt_get(opts, "session");
   if (s)
      return s;
   s = getenv("AIMEE_SESSION_ID");
   if (s && s[0])
      return s;
   return "default";
}

static cJSON *wm_entry_to_json(const wm_entry_t *e)
{
   cJSON *j = cJSON_CreateObject();
   cJSON_AddNumberToObject(j, "id", (double)e->id);
   cJSON_AddStringToObject(j, "session_id", e->session_id);
   cJSON_AddStringToObject(j, "key", e->key);
   cJSON_AddStringToObject(j, "value", e->value);
   cJSON_AddStringToObject(j, "category", e->category);
   cJSON_AddStringToObject(j, "created_at", e->created_at);
   cJSON_AddStringToObject(j, "updated_at", e->updated_at);
   if (e->expires_at[0])
      cJSON_AddStringToObject(j, "expires_at", e->expires_at);
   else
      cJSON_AddNullToObject(j, "expires_at");
   return j;
}

/* --- Subcommand handlers --- */

static void wm_set_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *key = opt_pos(&opts, 0);
   const char *value = opt_pos(&opts, 1);
   if (!key || !value)
      fatal("wm set requires <key> <value>");

   const char *session_id = resolve_session(&opts);
   const char *category = opt_get(&opts, "category");
   int ttl = opt_get_int(&opts, "ttl", WM_DEFAULT_TTL);

   if (db1_wm_set(session_id, key, value, category, ttl) != 0)
      fatal("failed to set working memory key: %s", key);

   if (ctx->json_output)
   {
      wm_entry_t entry;
      if (db1_wm_get(session_id, key, &entry) == 0)
         emit_json_ctx(wm_entry_to_json(&entry), ctx->json_fields, ctx->response_profile);
      else
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("set %s = %s\n", key, value);
   }
}

static void wm_get_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *key = opt_pos(&opts, 0);
   if (!key)
      fatal("wm get requires <key>");

   const char *session_id = resolve_session(&opts);

   wm_entry_t entry;
   if (db1_wm_get(session_id, key, &entry) != 0)
      fatal("key not found or expired: %s", key);

   if (ctx->json_output)
   {
      emit_json_ctx(wm_entry_to_json(&entry), ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("%s\n", entry.value);
   }
}

static void wm_list_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *session_id = resolve_session(&opts);
   const char *category = opt_get(&opts, "category");

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = db1_wm_list(session_id, category, entries, WM_MAX_RESULTS);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
         cJSON_AddItemToArray(arr, wm_entry_to_json(&entries[i]));
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      for (int i = 0; i < count; i++)
      {
         printf("[%s] %s: %s\n", entries[i].category, entries[i].key, entries[i].value);
      }
      if (count == 0)
         printf("(no entries)\n");
   }
}

static void wm_delete_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *key = opt_pos(&opts, 0);
   if (!key)
      fatal("wm delete requires <key>");

   const char *session_id = resolve_session(&opts);

   if (db1_wm_delete(session_id, key) != 0)
      fatal("failed to delete key: %s", key);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("deleted %s\n", key);
}

static void wm_clear_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   (void)argc;

   const char *session_id = resolve_session(&opts);

   if (db1_wm_clear(session_id) != 0)
      fatal("failed to clear working memory");

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      printf("cleared working memory for session %s\n", session_id);
}

static void wm_gc_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   int removed = db1_wm_gc();

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddNumberToObject(j, "removed", removed);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("removed %d expired entries\n", removed);
   }
}

static void wm_context_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);

   const char *session_id = resolve_session(&opts);

   char *context = db1_wm_assemble_context(session_id);
   if (!context)
   {
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNullToObject(j, "context");
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("(no working memory)\n");
      }
      return;
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "context", context);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("%s", context);
   }
   free(context);
}

/* --- Subcommand table --- */

static const subcmd_t wm_subcmds[] = {
    {"set", "Set a working memory key-value pair", wm_set_cmd},
    {"get", "Get a working memory value by key", wm_get_cmd},
    {"list", "List all working memory entries", wm_list_cmd},
    {"delete", "Delete a working memory entry", wm_delete_cmd},
    {"clear", "Clear all working memory", wm_clear_cmd},
    {"gc", "Garbage-collect expired entries", wm_gc_cmd},
    {"context", "Print assembled working memory context", wm_context_cmd},
    {NULL, NULL, NULL},
};

const subcmd_t *get_wm_subcmds(void)
{
   return wm_subcmds;
}

/* --- cmd_wm --- */

void cmd_wm(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("wm", wm_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   cmd_require_db1("cannot initialize DB1");

   if (subcmd_dispatch(wm_subcmds, sub, ctx, argc, argv) != 0)
      fatal("unknown wm subcommand: %s", sub);
}
