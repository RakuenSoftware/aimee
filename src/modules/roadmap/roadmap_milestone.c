/* roadmap_milestone.c: milestone validation gate for spec-driven-roadmap dispatch.
 *
 * See src/headers/roadmap_milestone.h.
 */

#include "aimee.h"
#include "roadmap_milestone.h"
#include "db1/roadmap_runtime.h"
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

/* ── verdict parsing ──────────────────────────────────────────────────────── */

const char *roadmap_milestone_parse_verdict(const char *review_text)
{
   if (!review_text)
      return "unknown";

   /* Scan in-place: convert to lowercase in a local copy so the caller's
    * string is not modified. */
   size_t len = strlen(review_text);
   char *lower = malloc(len + 1);
   if (!lower)
      return "unknown";

   for (size_t i = 0; i < len; i++)
      lower[i] = (char)tolower((unsigned char)review_text[i]);
   lower[len] = '\0';

   /* Priority: pass > partial > fail. */
   if (strstr(lower, "pass"))
   {
      free(lower);
      return "pass";
   }
   if (strstr(lower, "partial"))
   {
      free(lower);
      return "partial";
   }
   if (strstr(lower, "fail"))
   {
      free(lower);
      return "fail";
   }

   free(lower);
   return "unknown";
}

/* ── completion check ─────────────────────────────────────────────────────── */

int roadmap_milestone_all_done(const char *roadmap_id, const char *parent_id, const char *level)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT id, payload FROM artifacts "
                                          "WHERE kind = ?1 AND scope_id = ?2 AND state = ?3",
                                          err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", "plan_unit");
   aimee_pg_bind_text(st, "?2", roadmap_id);
   aimee_pg_bind_text(st, "?3", "committed");

   int found_any = 0;

   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *uid = aimee_pg_column_text(st, 0);
      const char *pl = aimee_pg_column_text(st, 1);
      if (!uid || !pl)
         continue;

      cJSON *payload = cJSON_Parse(pl);
      if (!payload)
         continue;

      cJSON *pid_j = cJSON_GetObjectItemCaseSensitive(payload, "parent_id");
      cJSON *lvl_j = cJSON_GetObjectItemCaseSensitive(payload, "level");
      if (!cJSON_IsString(pid_j) || !cJSON_IsString(lvl_j))
      {
         cJSON_Delete(payload);
         continue;
      }

      /* Filter to the requested parent_id and level. */
      if (strcmp(pid_j->valuestring, parent_id) != 0 || strcmp(lvl_j->valuestring, level) != 0)
      {
         cJSON_Delete(payload);
         continue;
      }

      found_any = 1;

      /* Check DB1 state. */
      rdm_unit_dispatch_t u;
      if (rdm_unit_get(roadmap_id, uid, &u) != 0)
      {
         cJSON_Delete(payload);
         aimee_pg_finalize(st);
         return -1;
      }

      if (strcmp(u.state, "done") != 0)
      {
         cJSON_Delete(payload);
         aimee_pg_finalize(st);
         return 0;
      }

      cJSON_Delete(payload);
   }

   aimee_pg_finalize(st);

   /* No units at all means not done. */
   if (!found_any)
      return 0;

   return 1;
}

/* ── review prompt builder ─────────────────────────────────────────────────── */

char *roadmap_milestone_review_prompt(const char *roadmap_id, const char *milestone_id)
{
   db2_artifact_row_t row;
   memset(&row, 0, sizeof(row));

   if (db2_artifact_read(milestone_id, &row, NULL, 0, NULL) != 0)
      return NULL;

   cJSON *payload = cJSON_Parse(row.payload_json);
   if (!payload)
      return NULL;

   const char *title = "";
   const char *intent = "";
   cJSON *tj = cJSON_GetObjectItemCaseSensitive(payload, "title");
   cJSON *ij = cJSON_GetObjectItemCaseSensitive(payload, "intent");
   if (cJSON_IsString(tj))
      title = tj->valuestring;
   if (cJSON_IsString(ij))
      intent = ij->valuestring;

   dstr_t s;
   dstr_init(&s);

   dstr_appendf(&s, "Review milestone: %s\n\n", title);

   if (intent[0])
      dstr_appendf(&s, "Intent: %s\n\n", intent);

   dstr_append_str(&s, "Acceptance criteria:\n");
   cJSON *ac = cJSON_GetObjectItemCaseSensitive(payload, "acceptance_criteria");
   if (cJSON_IsArray(ac))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, ac)
      {
         if (cJSON_IsString(item))
            dstr_appendf(&s, "- %s\n", item->valuestring);
      }
   }

   dstr_append_str(&s, "\n");
   dstr_append_str(&s, "Return a verdict of: pass, partial, or fail.\n");
   dstr_append_str(&s, "- pass: all acceptance criteria are met\n");
   dstr_append_str(&s, "- partial: most criteria met but gaps remain\n");
   dstr_append_str(&s, "- fail: critical acceptance criteria not met\n\n");
   dstr_append_str(&s, "Your response MUST include one of the words: pass, partial, or fail.\n");

   cJSON_Delete(payload);

   char *result = strdup(dstr_cstr(&s));
   dstr_free(&s);
   return result;
}

/* ── validation gate ──────────────────────────────────────────────────────── */

char *roadmap_milestone_validate(const char *roadmap_id, const char *milestone_id,
                                 agent_config_t *cfg)
{
   if (!cfg)
      return NULL;

   char *prompt = roadmap_milestone_review_prompt(roadmap_id, milestone_id);
   if (!prompt)
      return NULL;

   agent_result_t result;
   memset(&result, 0, sizeof(result));

   agent_http_init();
   int rc = agent_run(cfg, "review", NULL, prompt, 0, &result);
   agent_http_cleanup();

   free(prompt);

   if (rc != 0 || !result.response)
   {
      free(result.response);
      return NULL;
   }

   const char *verdict = roadmap_milestone_parse_verdict(result.response);
   char *ret = strdup(verdict);
   free(result.response);
   return ret;
}
