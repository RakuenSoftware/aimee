#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "trajectory.h"
#include "db1_client/interaction_events.h"

static const char *reply_override;
static int dispatch_status = 1;

int db1_interaction_event_list_for_session(const char *session, ie_event_row_t *out, int max)
{
   assert(max > 0 && strcmp(session, "fixture") == 0);
   memset(out, 0, sizeof(*out));
   strcpy(out->event_type, "agent_turn");
   strcpy(out->payload, "{\"content\":\"fixture-secret\"}");
   strcpy(out->created_at, "2026-09-17T00:00:00Z");
   return 1;
}

cJSON *audit_ledger_read(const char *since, const char *until)
{
   (void)since;
   (void)until;
   return NULL;
}

int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.screen_content") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(args, "capacity")->valueint == 8192);
   if (reply_override || dispatch_status != 1)
   {
      *result = reply_override ? cJSON_Parse(reply_override) : NULL;
      return dispatch_status;
   }
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));
   assert(content);
   *result =
       cJSON_Parse(strcmp(content, "fixture-secret") == 0
                       ? "{\"status\":\"ok\",\"verdict\":\"redact\",\"redacted\":\"[REDACTED]\"}"
                       : "{\"status\":\"ok\",\"verdict\":\"allow\"}");
   return 1;
}

int main(void)
{
   trajectory_opts_t opts = {.redact = 1};
   char *out = NULL;
   assert(trajectory_export("fixture", &opts, &out) == 0);
   assert(out && strstr(out, "[REDACTED]") && !strstr(out, "fixture-secret"));
   free(out);
   const char *replies[] = {"{}", "{\"status\":\"ok\",\"verdict\":\"reject\"}",
                            "{\"status\":\"ok\",\"verdict\":\"unknown\"}",
                            "{\"status\":\"error\",\"verdict\":\"allow\"}",
                            "{\"status\":\"ok\",\"verdict\":\"redact\"}"};
   for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++)
   {
      reply_override = replies[i];
      assert(trajectory_export("fixture", &opts, &out) == -1 && out == NULL);
   }
   reply_override = NULL;
   dispatch_status = -1;
   assert(trajectory_export("fixture", &opts, &out) == -1 && out == NULL);
   dispatch_status = 0;
   assert(trajectory_export("fixture", &opts, &out) == -1 && out == NULL);
   return 0;
}
