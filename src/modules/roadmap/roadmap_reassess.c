/* roadmap_reassess.c: post-completion reassessment pass for spec-driven roadmap loop.
 *
 * See src/headers/roadmap_reassess.h.
 */

#include "aimee.h"
#include "roadmap_reassess.h"
#include "db2/artifacts.h"
#include "db2/db_postgres.h"
#include "db2/db2_internal.h"
#include "headers/agent_exec.h"
#include "headers/agent_config.h"
#include "headers/dstr.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── prompt builder ───────────────────────────────────────────────────────── */

char *roadmap_reassess_build_prompt(const char *roadmap_id)
{
   if (!roadmap_id)
      return NULL;

   db2_artifact_row_t row;
   memset(&row, 0, sizeof(row));

   if (db2_artifact_read(roadmap_id, &row, NULL, 0, NULL) != 0)
      return NULL;

   cJSON *payload = cJSON_Parse(row.payload_json);
   if (!payload)
      return NULL;

   cJSON *goal_j = cJSON_GetObjectItemCaseSensitive(payload, "goal");
   const char *goal = cJSON_IsString(goal_j) ? goal_j->valuestring : "";

   dstr_t s;
   dstr_init(&s);

   dstr_append_str(
       &s, "You are reviewing a completed software roadmap to determine if all goals are met.\n\n");
   dstr_appendf(&s, "Goal: %s\n\n", goal);
   dstr_append_str(&s, "All milestone acceptance criteria:\n");

   void *conn = db2_conn();
   if (conn)
   {
      char err[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                             "SELECT id, payload FROM artifacts "
                                             "WHERE kind = ?1 AND scope_id = ?2 AND state = ?3",
                                             err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_text(st, "?1", "plan_unit");
         aimee_pg_bind_text(st, "?2", roadmap_id);
         aimee_pg_bind_text(st, "?3", "committed");

         while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *pl = aimee_pg_column_text(st, 1);
            if (!pl)
               continue;

            cJSON *mu = cJSON_Parse(pl);
            if (!mu)
               continue;

            cJSON *lvl_j = cJSON_GetObjectItemCaseSensitive(mu, "level");
            if (!cJSON_IsString(lvl_j) || strcmp(lvl_j->valuestring, "milestone") != 0)
            {
               cJSON_Delete(mu);
               continue;
            }

            cJSON *title_j = cJSON_GetObjectItemCaseSensitive(mu, "title");
            const char *title = cJSON_IsString(title_j) ? title_j->valuestring : "untitled";
            dstr_appendf(&s, "Milestone: %s\n", title);

            cJSON *ac = cJSON_GetObjectItemCaseSensitive(mu, "acceptance_criteria");
            if (cJSON_IsArray(ac))
            {
               cJSON *item;
               cJSON_ArrayForEach(item, ac)
               {
                  if (cJSON_IsString(item))
                     dstr_appendf(&s, "  - %s\n", item->valuestring);
               }
            }

            cJSON_Delete(mu);
         }

         aimee_pg_finalize(st);
      }
   }

   cJSON_Delete(payload);

   dstr_append_str(&s, "\nAre all goals and acceptance criteria fully met? ");
   dstr_append_str(&s, "If YES: respond with 'complete' and a brief summary. ");
   dstr_append_str(&s, "If NO: respond with 'gaps found' and list the specific gaps.\n");

   char *result = strdup(dstr_cstr(&s));
   dstr_free(&s);
   return result;
}

/* ── verdict parser ───────────────────────────────────────────────────────── */

int roadmap_reassess_is_complete(const char *response_text)
{
   if (!response_text)
      return 0;

   size_t len = strlen(response_text);
   char *lower = malloc(len + 1);
   if (!lower)
      return 0;

   for (size_t i = 0; i < len; i++)
      lower[i] = (char)tolower((unsigned char)response_text[i]);
   lower[len] = '\0';

   int ok = (strstr(lower, "complete") != NULL || strstr(lower, "no gaps") != NULL ||
             strstr(lower, "all goals met") != NULL);

   free(lower);
   return ok;
}

/* ── reassessment runner ──────────────────────────────────────────────────── */

int roadmap_reassess_run(const char *roadmap_id, agent_config_t *cfg, char **out_verdict)
{
   if (!cfg || !out_verdict)
      return -1;

   *out_verdict = NULL;

   char *prompt = roadmap_reassess_build_prompt(roadmap_id);
   if (!prompt)
      return -1;

   agent_result_t result;
   memset(&result, 0, sizeof(result));

   agent_http_init();
   int rc = agent_run(cfg, "reason", NULL, prompt, 0, &result);
   agent_http_cleanup();

   free(prompt);

   if (rc != 0 || !result.response)
   {
      free(result.response);
      return -1;
   }

   *out_verdict = result.response;
   int complete = roadmap_reassess_is_complete(*out_verdict);
   return complete ? 0 : 1;
}
