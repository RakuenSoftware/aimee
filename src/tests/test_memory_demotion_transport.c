/* The native endpoint snapshots host configuration and forwards the Go result. */
#include "cJSON.h"
#include "kb_service_agent.h"
#include "kb_intel_payload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int mode = 2, transport_result = 1, calls;
static const char *reply;
static const char *operation = "demotion-run";
static cJSON *sent;

int config_demotion_enabled(void)
{
   return mode;
}
int config_demotion_n_min(void)
{
   return 7;
}
int config_demotion_window(void)
{
   return 41;
}
double config_demotion_half_life_days(void)
{
   return 12.5;
}

int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   assert(strcmp(method, "memory.runtime") == 0 && timeout_ms == 120000);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "operation")),
                 operation) == 0);
   const cJSON *config = cJSON_GetObjectItemCaseSensitive(args, "config");
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(config, "enabled")) == mode);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(config, "n_min")) == 7);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(config, "window")) == 41);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(config, "half_life_days")) == 12.5);
   *result = reply ? cJSON_Parse(reply) : NULL;
   calls++;
   return transport_result;
}
int kb_send_response(int fd, cJSON *response)
{
   assert(fd == 17);
   cJSON_Delete(sent);
   sent = cJSON_Duplicate(response, 1);
   return 29;
}
int main(void)
{
   cJSON *request = cJSON_Parse("{\"config\":{\"enabled\":999,\"n_min\":1}}");
   reply = "{\"status\":\"ok\",\"profiles_written\":3,\"profiles_created\":1,\"demoted\":2,"
           "\"scored\":9,\"skipped\":false}";
   assert(kb_handle_maintenance_compute_demotions(17, request) == 29);
   cJSON *expected = cJSON_Parse(reply);
   assert(cJSON_Compare(sent, expected, 1));
   cJSON_Delete(expected);
   mode = 0;
   reply = "{\"status\":\"ok\",\"profiles_written\":0}";
   assert(kb_handle_maintenance_compute_demotions(17, request) == 29);
   assert(calls == 2);
   const char *failures[] = {NULL, "[]", "{}", "{\"status\":\"error\"}"};
   for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++)
   {
      reply = failures[i];
      assert(kb_handle_maintenance_compute_demotions(17, request) == 29);
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "status")),
                    "error") == 0);
      assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(sent, "profiles_written")) == 0);
   }
   transport_result = -1;
   reply = "{\"status\":\"ok\",\"profiles_written\":100}";
   assert(kb_handle_maintenance_compute_demotions(17, request) == 29);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "status")), "error") ==
          0);
   operation = "demotion-check";
   transport_result = 1;
   reply = "{\"status\":\"ok\",\"candidates\":3,\"scored\":2,\"would_demote\":1,\"by_kind\":[]}";
   cJSON *preview = kb_intel_demote_check_response();
   expected = cJSON_Parse(reply);
   assert(preview && cJSON_Compare(preview, expected, 1));
   cJSON_Delete(preview);
   cJSON_Delete(expected);
   for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++)
   {
      reply = failures[i];
      assert(kb_intel_demote_check_response() == NULL);
   }
   reply = "{\"status\":\"ok\"}";
   transport_result = -1;
   assert(kb_intel_demote_check_response() == NULL);
   cJSON_Delete(sent);
   cJSON_Delete(request);
   puts("demotion native transport: ok");
   return 0;
}
