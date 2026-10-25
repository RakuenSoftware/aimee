/* roadmap_reassess.c: post-completion reassessment pass for spec-driven roadmap loop.
 *
 * See src/headers/roadmap_reassess.h.
 */

#include "aimee.h"
#include "roadmap_reassess.h"
#include "modules/kb/c/artifacts.h"
#include "modules/kb/c/db_postgres.h"
#include "modules/kb/c/kb_store_internal.h"
#include "headers/agent_exec.h"
#include "headers/agent_config.h"
#include "headers/dstr.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── prompt builder ───────────────────────────────────────────────────────── */

int roadmap_reassess_build_request(const char *roadmap_id, aimee_request_t *out)
{
   if (!out || !roadmap_id)
      return -1;

   kb_store_artifact_row_t row;
   memset(&row, 0, sizeof(row));

   if (kb_store_artifact_read(roadmap_id, &row, NULL, 0, NULL) != 0)
      return -1;

   cJSON *payload = cJSON_Parse(row.payload_json);
   if (!payload)
      return -1;

   cJSON *goal_j = cJSON_GetObjectItemCaseSensitive(payload, "goal");
   const char *goal = cJSON_IsString(goal_j) ? goal_j->valuestring : "";

   dstr_t s;
   dstr_init(&s);

   dstr_appendf(&s, "Goal: %s\n\n", goal);
   dstr_append_str(&s, "All milestone acceptance criteria:\n");

   void *conn = kb_store_conn();
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

   aimee_block_t policy = {
       .type = AIMEE_BLK_TEXT,
       .text =
           "You are reviewing a completed software roadmap to determine if all goals are met.\n\n"
           "Are all goals and acceptance criteria fully met? "
           "If YES: respond with 'complete' and a brief summary. "
           "If NO: respond with 'gaps found' and list the specific gaps.\n"};
   aimee_block_t evidence = {.type = AIMEE_BLK_TEXT, .text = (char *)dstr_cstr(&s)};
   aimee_ir_block_set_context(&policy, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL, 0);
   aimee_ir_block_set_context(&evidence, AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_message_t message = {.role = "user", .blocks = &evidence, .n_blocks = 1};
   aimee_request_t request = {.system = &policy,
                              .n_system = 1,
                              .messages = &message,
                              .n_messages = 1,
                              .has_temperature = 1,
                              .temperature = 0.3};
   int rc = aimee_ir_request_clone(&request, out);
   dstr_free(&s);
   return rc;
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

   aimee_request_t request = {0};
   if (roadmap_reassess_build_request(roadmap_id, &request))
      return -1;

   agent_result_t result = {0};
   memset(&result, 0, sizeof(result));

   agent_http_init();
   int rc = agent_run(cfg, "reason", &request, &result);
   aimee_request_free(&request);
   agent_http_cleanup();

   char *authored = rc ? NULL : agent_result_take_text(&result);
   if (!authored)
   {
      agent_result_clear_response(&result);
      return -1;
   }

   *out_verdict = authored;
   int complete = roadmap_reassess_is_complete(*out_verdict);
   return complete ? 0 : 1;
}
