/* cmd_review.c: shared review loop for charter artifact surfaces.
 *
 * Implements:
 *   cmd_review_surface(surface, ctx, argc, argv)  — per-surface review
 *   cmd_review_all(ctx, argc, argv)               — cross-surface shorthand
 *
 * Options: --limit N  --confidence-min F  --json
 * Interactive: [y]es / [n]o <reason> / [s]kip / [a]ll-high / [q]uit
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cmd_review.h"
#include "aimee.h"
#include "config.h"
#include "kb_client.h"
#include "log.h"
#include <aimee/skills/skill.h>
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVIEW_DEFAULT_LIMIT  20
#define REVIEW_BULK_THRESHOLD 0.85

/* ------------------------------------------------------------------ helpers */

static void parse_opts(int argc, char **argv, int *limit, double *conf_min, int *json_mode)
{
   *limit = REVIEW_DEFAULT_LIMIT;
   *conf_min = 0.0;
   *json_mode = 0;

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
      {
         *json_mode = 1;
      }
      else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
      {
         int v = atoi(argv[++i]);
         if (v > 0)
            *limit = v;
      }
      else if (strcmp(argv[i], "--confidence-min") == 0 && i + 1 < argc)
      {
         *conf_min = atof(argv[++i]);
      }
   }
}

/* Strip leading whitespace from a string. */
static const char *ltrim(const char *s)
{
   while (s && (*s == ' ' || *s == '\t'))
      s++;
   return s;
}

/* Render one artifact for the operator. */
static void render_artifact(const cJSON *item, int idx, int total, const char *surface)
{
   const char *id = "";
   const char *kind = "";
   const char *surf = surface ? surface : "";
   double conf = 0.0;
   const char *created = "";
   const char *payload = "{}";

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
   cJSON *jkind = cJSON_GetObjectItemCaseSensitive(item, "kind");
   cJSON *jsurf = cJSON_GetObjectItemCaseSensitive(item, "target_surface");
   cJSON *jconf = cJSON_GetObjectItemCaseSensitive(item, "confidence");
   cJSON *jcreated = cJSON_GetObjectItemCaseSensitive(item, "created_at");
   cJSON *jpayload = cJSON_GetObjectItemCaseSensitive(item, "payload_json");

   if (cJSON_IsString(jid))
      id = jid->valuestring;
   if (cJSON_IsString(jkind))
      kind = jkind->valuestring;
   if (cJSON_IsString(jsurf) && jsurf->valuestring[0])
      surf = jsurf->valuestring;
   if (cJSON_IsNumber(jconf))
      conf = jconf->valuedouble;
   if (cJSON_IsString(jcreated))
      created = jcreated->valuestring;
   if (cJSON_IsString(jpayload))
      payload = jpayload->valuestring;

   printf("\n[%d/%d] %s  surface=%-18s  confidence=%.2f  created=%s\n", idx, total, id, surf, conf,
          created);
   printf("  kind: %s\n", kind);

   /* Pretty-print payload */
   cJSON *p = cJSON_Parse(payload);
   if (p)
   {
      char *pretty = cJSON_Print(p);
      if (pretty)
      {
         printf("  payload:\n");
         const char *line = pretty;
         while (*line)
         {
            const char *nl = strchr(line, '\n');
            int len = nl ? (int)(nl - line) : (int)strlen(line);
            printf("    %.*s\n", len, line);
            if (!nl)
               break;
            line = nl + 1;
         }
         free(pretty);
      }
      cJSON_Delete(p);
   }
   else
   {
      printf("  payload: %s\n", payload);
   }
}

static int review_skill_gate_allows(const cJSON *item, int operator_override)
{
   cJSON *jkind = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "kind");
   if (!cJSON_IsString(jkind) || strcmp(jkind->valuestring, "skill_change") != 0)
      return 1;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.skills_eval_gate_enabled)
      return 1;

   cJSON *jpayload = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "payload_json");
   const char *payload = cJSON_IsString(jpayload) ? jpayload->valuestring : "";
   char reason[256];
   if (skill_change_eval_gate_allows(payload, cfg.skills_eval_threshold, reason, sizeof(reason)))
      return 1;

   if (operator_override)
   {
      fprintf(stderr, "  warning: %s; accepting by operator override\n",
              reason[0] ? reason : "skill eval gate blocked promotion");
      return 1;
   }

   fprintf(stderr, "  blocked: %s\n", reason[0] ? reason : "skill eval gate blocked promotion");
   return 0;
}

/* Accept a single artifact. */
static int do_accept(const cJSON *item, const char *id, int operator_override)
{
   if (!review_skill_gate_allows(item, operator_override))
      return -1;

   char *resp = kb_client_artifact_set_state_json(id, "committed", NULL, NULL, NULL, "operator");
   if (!resp)
   {
      fprintf(stderr, "  error: no response from kb service\n");
      return -1;
   }
   cJSON *r = cJSON_Parse(resp);
   free(resp);
   int ok = 0;
   if (r)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
      ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
      if (!ok)
      {
         cJSON *err = cJSON_GetObjectItemCaseSensitive(r, "error");
         fprintf(stderr, "  error: %s\n",
                 cJSON_IsString(err) ? err->valuestring : "set_state failed");
      }
      cJSON_Delete(r);
   }
   return ok ? 0 : -1;
}

/* Reject a single artifact with a reason string. */
static int do_reject(const char *id, const char *reason)
{
   /* Parse simple "tag: explanation" format from reason */
   char tag[64] = "";
   const char *colon = reason ? strchr(reason, ':') : NULL;
   if (colon && colon - reason < 60)
   {
      snprintf(tag, sizeof(tag), "%.*s", (int)(colon - reason), reason);
      reason = ltrim(colon + 1);
   }

   char *resp = kb_client_artifact_set_state_json(id, "rejected", tag[0] ? tag : NULL, NULL,
                                                  reason && reason[0] ? reason : NULL, "operator");
   if (!resp)
   {
      fprintf(stderr, "  error: no response from kb service\n");
      return -1;
   }
   cJSON *r = cJSON_Parse(resp);
   free(resp);
   int ok = 0;
   if (r)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
      ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
      if (!ok)
      {
         cJSON *err = cJSON_GetObjectItemCaseSensitive(r, "error");
         fprintf(stderr, "  error: %s\n",
                 cJSON_IsString(err) ? err->valuestring : "set_state failed");
      }
      cJSON_Delete(r);
   }
   return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ loop */

static void run_review_loop(cJSON *items, int json_mode, double conf_min)
{
   if (!items || !cJSON_IsArray(items))
   {
      printf("No proposed artifacts.\n");
      return;
   }

   int total = cJSON_GetArraySize(items);
   if (total == 0)
   {
      printf("No proposed artifacts.\n");
      return;
   }

   if (json_mode)
   {
      char *out = cJSON_PrintUnformatted(items);
      if (out)
      {
         printf("%s\n", out);
         free(out);
      }
      return;
   }

   printf("Reviewing %d proposed artifact(s).  [y]es [n]o <reason> [s]kip [a]ll-high [q]uit\n",
          total);

   int accepted = 0, rejected = 0, skipped = 0;

   int idx = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, items)
   {
      idx++;

      cJSON *jconf = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      double conf = cJSON_IsNumber(jconf) ? jconf->valuedouble : 0.0;
      if (conf < conf_min)
      {
         skipped++;
         continue;
      }

      cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
      const char *id = cJSON_IsString(jid) ? jid->valuestring : "";

      render_artifact(item, idx, total, NULL);
      printf("  > ");
      fflush(stdout);

      char line[512] = "";
      if (!fgets(line, sizeof(line), stdin))
         break;

      /* Strip trailing newline */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      const char *cmd = ltrim(line);
      if (!cmd || !cmd[0])
         cmd = "s";

      if (cmd[0] == 'q' || cmd[0] == 'Q')
      {
         printf("Quitting review.\n");
         break;
      }
      else if (cmd[0] == 'y' || cmd[0] == 'Y')
      {
         if (do_accept(item, id, 1) == 0)
         {
            printf("  accepted.\n");
            accepted++;
         }
      }
      else if (cmd[0] == 'n' || cmd[0] == 'N')
      {
         const char *reason = ltrim(cmd + 1);
         if (do_reject(id, reason) == 0)
         {
            printf("  rejected.\n");
            rejected++;
         }
      }
      else if (cmd[0] == 's' || cmd[0] == 'S')
      {
         skipped++;
      }
      else if (cmd[0] == 'a' || cmd[0] == 'A')
      {
         /* Bulk-accept all remaining with confidence >= threshold */
         printf("Bulk-accepting artifacts with confidence >= %.2f ...\n", REVIEW_BULK_THRESHOLD);
         /* Accept the current item if it qualifies */
         if (conf >= REVIEW_BULK_THRESHOLD)
         {
            if (do_accept(item, id, 0) == 0)
            {
               printf("  %s accepted.\n", id);
               accepted++;
            }
         }
         /* Accept remaining items */
         cJSON *rest = item->next;
         while (rest)
         {
            cJSON *rconf = cJSON_GetObjectItemCaseSensitive(rest, "confidence");
            cJSON *rid = cJSON_GetObjectItemCaseSensitive(rest, "id");
            double rc = cJSON_IsNumber(rconf) ? rconf->valuedouble : 0.0;
            const char *rid_s = cJSON_IsString(rid) ? rid->valuestring : "";
            if (rc >= REVIEW_BULK_THRESHOLD && rid_s[0])
            {
               if (do_accept(rest, rid_s, 0) == 0)
               {
                  printf("  %s accepted (bulk).\n", rid_s);
                  accepted++;
               }
            }
            else
            {
               skipped++;
            }
            rest = rest->next;
         }
         break;
      }
   }

   printf("\nReview summary: accepted=%d  rejected=%d  skipped=%d\n", accepted, rejected, skipped);
}

/* ------------------------------------------------------------------ public */

void cmd_review_surface(const char *surface, app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   int limit, json_mode;
   double conf_min;
   parse_opts(argc, argv, &limit, &conf_min, &json_mode);

   char *raw = kb_client_artifacts_list_proposed_json(surface, limit);
   if (!raw)
   {
      fprintf(stderr, "error: knowledge service unavailable\n");
      return;
   }

   cJSON *resp = cJSON_Parse(raw);
   free(raw);
   if (!resp)
   {
      fprintf(stderr, "error: invalid response from knowledge service\n");
      return;
   }

   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "artifacts");
   if (!items)
      items = resp; /* bare array fallback */

   run_review_loop(items, json_mode, conf_min);
   cJSON_Delete(resp);
}

/* Is |name| one of the charter promotion target surfaces? Lets
 * `aimee review <surface>` review a single surface with the same flags. */
static int is_review_surface(const char *name)
{
   static const char *const surfaces[] = {
       "memory", "anti_pattern",        "workflow_pattern",   "rule",
       "entity", "epistemic_directive", "guardrail_exemplar", "working_profile",
   };
   if (!name)
      return 0;
   for (size_t i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); i++)
      if (strcmp(name, surfaces[i]) == 0)
         return 1;
   return 0;
}

void cmd_review_all(app_ctx_t *ctx, int argc, char **argv)
{
   /* `aimee review <surface> [flags]` reviews a single promotion target surface
    * with the identical flag shape; `aimee review [flags]` is cross-surface. */
   if (argc >= 1 && is_review_surface(argv[0]))
   {
      cmd_review_surface(argv[0], ctx, argc - 1, argv + 1);
      return;
   }

   (void)ctx;
   int limit, json_mode;
   double conf_min;
   parse_opts(argc, argv, &limit, &conf_min, &json_mode);

   /* cross-surface: pass NULL surface, larger default limit */
   if (limit == REVIEW_DEFAULT_LIMIT)
      limit = 50;

   char *raw = kb_client_artifacts_list_proposed_json(NULL, limit);
   if (!raw)
   {
      fprintf(stderr, "error: knowledge service unavailable\n");
      return;
   }

   cJSON *resp = cJSON_Parse(raw);
   free(raw);
   if (!resp)
   {
      fprintf(stderr, "error: invalid response from knowledge service\n");
      return;
   }

   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "artifacts");
   if (!items)
      items = resp;

   run_review_loop(items, json_mode, conf_min);
   cJSON_Delete(resp);
}
