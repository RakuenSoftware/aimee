/* cmd_roadmap.c: roadmap CLI (new, show, list, status, rebuild, validate) */
#include "aimee.h"
#include "log.h"
#include "commands.h"
#include "kb_client.h"
#include "cJSON.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "roadmap_decompose.h"

/* Model hook for roadmap_decompose_run: wraps agent_run with the "draft"
 * role. ud is a pointer to an already-loaded agent_config_t. */
static int roadmap_model_via_agent(const char *prompt, char **out_text, void *ud)
{
   agent_config_t *cfg = (agent_config_t *)ud;
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run(cfg, "draft", NULL, prompt, 0, &result);
   if (rc != 0 || !result.success || !result.response || !result.response[0])
   {
      free(result.response);
      return -1;
   }
   *out_text = result.response; /* transfer ownership */
   return 0;
}

/* Read entire file into a heap buffer. Returns NULL on failure. */
static char *read_whole_file(const char *path)
{
   if (strcmp(path, "-") == 0)
   {
      char buf[8192];
      size_t total = 0;
      size_t cap = 0;
      char *out = NULL;
      for (;;)
      {
         size_t n = fread(buf, 1, sizeof(buf), stdin);
         if (n == 0)
            break;
         if (total + n > cap)
         {
            cap = (total + n) * 2;
            char *tmp = realloc(out, cap);
            if (!tmp)
            {
               free(out);
               return NULL;
            }
            out = tmp;
         }
         memcpy(out + total, buf, n);
         total += n;
      }
      if (!out)
      {
         out = malloc(1);
         if (out)
            out[0] = '\0';
      }
      else
         out[total] = '\0';
      return out;
   }

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc(len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, len, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

/* Find --from value in argv, return pointer to the following arg or NULL. */
static const char *find_from_arg(int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--from") == 0 && i + 1 < argc)
         return argv[i + 1];
   }
   return NULL;
}

void cmd_roadmap(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("roadmap requires a subcommand: new, show, list, status, rebuild, validate, report");

   const char *sub = argv[0];
   argc--;
   argv++;

   if (strcmp(sub, "new") == 0)
   {
      const char *from = find_from_arg(argc, argv);
      char *decomp_json = NULL;

      if (from)
      {
         /* Manual path: read a pre-authored decomposition JSON document. */
         decomp_json = read_whole_file(from);
         if (!decomp_json)
            fatal("roadmap new: failed to read --from %s", from);
      }
      else
      {
         /* Delegate path: first positional arg is the bare goal string. */
         if (argc < 1)
            fatal("roadmap new requires a goal (\"aimee roadmap new \\\"<goal>\\\"\") "
                  "or --from <file> for a pre-authored decomposition document");
         const char *goal = argv[0];

         /* Deduce optional flags after the goal: --depth, --profile. */
         const char *depth = NULL, *profile = NULL;
         for (int i = 1; i < argc; i++)
         {
            if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc)
               depth = argv[++i];
            else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc)
               profile = argv[++i];
         }

         agent_config_t cfg;
         if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
            fatal("roadmap new: no agents configured");

         agent_http_init();
         char decomp_err[256] = "";
         int rc = roadmap_decompose_run(goal, depth, profile, roadmap_model_via_agent, &cfg, 3,
                                        &decomp_json, decomp_err, sizeof(decomp_err));
         agent_http_cleanup();

         if (rc != 0 || !decomp_json)
            fatal("roadmap new: decomposition failed: %s", decomp_err);
      }

      char *json = kb_client_roadmap_create_json(decomp_json);
      free(decomp_json);

      cJSON *resp = json ? cJSON_Parse(json) : NULL;
      free(json);

      if (!resp)
         fatal("roadmap new: parse failed");

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
      {
         cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
         cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
         const char *errstr = cJSON_IsString(err)
                                  ? err->valuestring
                                  : (cJSON_IsString(msg) ? msg->valuestring : "unknown error");
         fprintf(stderr, "error: %s\n", errstr);
         cJSON_Delete(resp);
         exit(1);
      }

      cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "roadmap_id");
      const char *idstr = cJSON_IsString(id) ? id->valuestring : "";

      if (ctx->json_output)
      {
         emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("created roadmap %s\n", idstr);
      }
      cJSON_Delete(resp);
   }
   else if (strcmp(sub, "show") == 0)
   {
      if (argc < 1)
         fatal("roadmap show requires an id");

      const char *id = argv[0];
      char *json = kb_client_roadmap_show_json(id);

      cJSON *resp = json ? cJSON_Parse(json) : NULL;
      free(json);

      if (!resp)
         fatal("roadmap show: parse failed");

      if (ctx->json_output)
      {
         emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         cJSON *rm = cJSON_GetObjectItemCaseSensitive(resp, "roadmap");
         cJSON *units = cJSON_GetObjectItemCaseSensitive(resp, "units");
         cJSON *sid = cJSON_GetObjectItemCaseSensitive(resp, "id");
         const char *ids = cJSON_IsString(sid) ? sid->valuestring : id;

         printf("roadmap %s\n", ids);
         if (cJSON_IsObject(rm))
         {
            cJSON *goal = cJSON_GetObjectItemCaseSensitive(rm, "goal");
            if (cJSON_IsString(goal) && goal->valuestring[0])
               printf("goal: %s\n", goal->valuestring);
         }
         if (cJSON_IsArray(units))
         {
            cJSON *item;
            cJSON_ArrayForEach(item, units)
            {
               cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "state");
               cJSON *lv = cJSON_GetObjectItemCaseSensitive(item, "level");
               cJSON *ti = cJSON_GetObjectItemCaseSensitive(item, "title");
               const char *state = cJSON_IsString(st) ? st->valuestring : "";
               const char *level = cJSON_IsString(lv) ? lv->valuestring : "";
               const char *title = cJSON_IsString(ti) ? ti->valuestring : "";
               printf("- [%s] %s: %s\n", state, level, title);
            }
         }
      }
      cJSON_Delete(resp);
   }
   else if (strcmp(sub, "list") == 0)
   {
      char *json = kb_client_roadmap_list_json();
      cJSON *resp = json ? cJSON_Parse(json) : NULL;
      free(json);

      if (!resp)
         fatal("roadmap list: parse failed");

      cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "roadmaps");
      if (ctx->json_output)
      {
         cJSON *out =
             cJSON_IsArray(arr) ? cJSON_DetachItemViaPointer(resp, arr) : cJSON_CreateArray();
         emit_json_ctx(out, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         if (cJSON_IsArray(arr))
         {
            cJSON *item;
            cJSON_ArrayForEach(item, arr)
            {
               cJSON *sid = cJSON_GetObjectItemCaseSensitive(item, "id");
               cJSON *goal = cJSON_GetObjectItemCaseSensitive(item, "goal");
               const char *ids = cJSON_IsString(sid) ? sid->valuestring : "";
               const char *gs = cJSON_IsString(goal) ? goal->valuestring : "";
               printf("%s  %s\n", ids, gs);
            }
         }
      }
      cJSON_Delete(resp);
   }
   else if (strcmp(sub, "status") == 0)
   {
      if (argc >= 1)
      {
         const char *id = argv[0];
         char *json = kb_client_roadmap_show_json(id);
         cJSON *resp = json ? cJSON_Parse(json) : NULL;
         free(json);

         if (!resp)
            fatal("roadmap status: parse failed");

         if (ctx->json_output)
         {
            emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
         }
         else
         {
            cJSON *rm = cJSON_GetObjectItemCaseSensitive(resp, "roadmap");
            cJSON *units = cJSON_GetObjectItemCaseSensitive(resp, "units");
            cJSON *sid = cJSON_GetObjectItemCaseSensitive(resp, "id");
            const char *ids = cJSON_IsString(sid) ? sid->valuestring : id;

            printf("roadmap %s\n", ids);
            if (cJSON_IsObject(rm))
            {
               cJSON *goal = cJSON_GetObjectItemCaseSensitive(rm, "goal");
               if (cJSON_IsString(goal) && goal->valuestring[0])
                  printf("goal: %s\n", goal->valuestring);
            }
            if (cJSON_IsArray(units))
            {
               cJSON *item;
               cJSON_ArrayForEach(item, units)
               {
                  cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "state");
                  cJSON *lv = cJSON_GetObjectItemCaseSensitive(item, "level");
                  cJSON *ti = cJSON_GetObjectItemCaseSensitive(item, "title");
                  const char *state = cJSON_IsString(st) ? st->valuestring : "";
                  const char *level = cJSON_IsString(lv) ? lv->valuestring : "";
                  const char *title = cJSON_IsString(ti) ? ti->valuestring : "";
                  printf("- [%s] %s: %s\n", state, level, title);
               }
            }
         }
         cJSON_Delete(resp);
      }
      else
      {
         char *json = kb_client_roadmap_list_json();
         cJSON *resp = json ? cJSON_Parse(json) : NULL;
         free(json);

         if (!resp)
            fatal("roadmap status: parse failed");

         cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "roadmaps");
         if (ctx->json_output)
         {
            cJSON *out =
                cJSON_IsArray(arr) ? cJSON_DetachItemViaPointer(resp, arr) : cJSON_CreateArray();
            emit_json_ctx(out, ctx->json_fields, ctx->response_profile);
         }
         else
         {
            if (cJSON_IsArray(arr))
            {
               cJSON *item;
               cJSON_ArrayForEach(item, arr)
               {
                  cJSON *sid = cJSON_GetObjectItemCaseSensitive(item, "id");
                  cJSON *goal = cJSON_GetObjectItemCaseSensitive(item, "goal");
                  const char *ids = cJSON_IsString(sid) ? sid->valuestring : "";
                  const char *gs = cJSON_IsString(goal) ? goal->valuestring : "";
                  printf("%s  %s\n", ids, gs);
               }
            }
         }
         cJSON_Delete(resp);
      }
   }
   else if (strcmp(sub, "rebuild") == 0)
   {
      if (argc < 1)
         fatal("roadmap rebuild requires an id");

      const char *id = argv[0];
      char *json = kb_client_roadmap_rebuild_json(id);

      cJSON *resp = json ? cJSON_Parse(json) : NULL;
      free(json);

      if (!resp)
         fatal("roadmap rebuild: parse failed");

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
      {
         cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
         cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
         const char *errstr = cJSON_IsString(err)
                                  ? err->valuestring
                                  : (cJSON_IsString(msg) ? msg->valuestring : "unknown error");
         fprintf(stderr, "error: %s\n", errstr);
         cJSON_Delete(resp);
         exit(1);
      }

      if (ctx->json_output)
      {
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("rebuilt projections for %s\n", id);
      }
      cJSON_Delete(resp);
   }
   else if (strcmp(sub, "validate") == 0)
   {
      const char *from = find_from_arg(argc, argv);
      if (!from)
         fatal("roadmap validate requires --from <file> (a decomposition JSON document)");

      char *buf = read_whole_file(from);
      if (!buf)
         fatal("roadmap validate: failed to read --from %s", from);

      char *json = kb_client_roadmap_validate_json(buf);
      free(buf);

      cJSON *resp = json ? cJSON_Parse(json) : NULL;
      free(json);

      if (!resp)
         fatal("roadmap validate: parse failed");

      if (ctx->json_output)
      {
         emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "valid");
         cJSON *reason = cJSON_GetObjectItemCaseSensitive(resp, "reason");
         int is_valid = cJSON_IsBool(v) && cJSON_IsTrue(v);
         const char *reasonstr = cJSON_IsString(reason) ? reason->valuestring : "";
         if (is_valid)
            printf("valid\n");
         else
            printf("invalid: %s\n", reasonstr);
      }
      cJSON_Delete(resp);
   }
   else if (strcmp(sub, "report") == 0)
   {
      if (argc < 1)
         fatal("roadmap report requires an id");

      const char *id = argv[0];
      const char *output_path = NULL;
      int is_html = 0;
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--html") == 0)
            is_html = 1;
         else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output_path = argv[++i];
      }
      (void)is_html; /* flag reserved; HTML is the only format for now */

      char *json = kb_client_roadmap_report_json(id, output_path);
      cJSON *resp = json ? cJSON_Parse(json) : NULL;
      free(json);

      if (!resp)
         fatal("roadmap report: parse failed");

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0)
      {
         cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
         cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
         const char *errstr = cJSON_IsString(err)
                                  ? err->valuestring
                                  : (cJSON_IsString(msg) ? msg->valuestring : "unknown error");
         fprintf(stderr, "error: %s\n", errstr);
         cJSON_Delete(resp);
         exit(1);
      }

      if (ctx->json_output)
      {
         emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         cJSON *rpath = cJSON_GetObjectItemCaseSensitive(resp, "report_path");
         printf("report written: %s\n", cJSON_IsString(rpath) ? rpath->valuestring : "(unknown)");
      }
      cJSON_Delete(resp);
   }
   else
   {
      fatal("unknown roadmap subcommand: %s (new, show, list, status, rebuild, validate, report)",
            sub);
   }
}