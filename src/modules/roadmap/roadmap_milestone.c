/* roadmap_milestone.c: milestone validation gate for spec-driven-roadmap dispatch.
 *
 * See src/headers/roadmap_milestone.h.
 */

#include "aimee.h"
#include "roadmap_milestone.h"
#include "db1_client/roadmap_runtime.h"
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
   void *conn = kb_store_conn();
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
      if (db1_roadmap_unit_get(roadmap_id, uid, &u) != 0)
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

int roadmap_milestone_build_request(const char *roadmap_id, const char *milestone_id,
                                    aimee_request_t *out)
{
   if (!out)
      return -1;
   kb_store_artifact_row_t row;
   memset(&row, 0, sizeof(row));

   if (kb_store_artifact_read(milestone_id, &row, NULL, 0, NULL) != 0)
      return -1;

   cJSON *payload = cJSON_Parse(row.payload_json);
   if (!payload)
      return -1;

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


   cJSON_Delete(payload);

   aimee_block_t policy = {
       .type = AIMEE_BLK_TEXT,
       .text = "Return a verdict of: pass, partial, or fail.\n"
               "- pass: all acceptance criteria are met\n"
               "- partial: most criteria met but gaps remain\n"
               "- fail: critical acceptance criteria not met\n\n"
               "Your response MUST include one of the words: pass, partial, or fail.\n"};
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

/* ── validation gate ──────────────────────────────────────────────────────── */

char *roadmap_milestone_validate(const char *roadmap_id, const char *milestone_id,
                                 agent_config_t *cfg)
{
   if (!cfg)
      return NULL;

   aimee_request_t request = {0};
   if (roadmap_milestone_build_request(roadmap_id, milestone_id, &request))
      return NULL;

   agent_result_t result = {0};
   memset(&result, 0, sizeof(result));

   agent_http_init();
   int rc = agent_run(cfg, "review", &request, &result);
   aimee_request_free(&request);
   agent_http_cleanup();

   char *authored = rc ? NULL : agent_result_take_text(&result);
   if (!authored)
   {
      agent_result_clear_response(&result);
      return NULL;
   }

   const char *verdict = roadmap_milestone_parse_verdict(authored);
   char *ret = strdup(verdict);
   free(authored);
   agent_result_clear_response(&result);
   return ret;
}
