/* Once the daemon receives config-changed (ouroboros ring D14), the native config
 * client answers "has it changed?" from the announcements instead of asking the
 * module for its version. The transport seam counts what reaches the module. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_client.h"
#include "module_json_call.h"

#define VERSION_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define VERSION_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static int snapshot_calls, version_calls;
static const char *current_version = VERSION_A;
/* Run inside a snapshot call, before it answers: an announcement in flight. */
static void (*during_snapshot)(void);

static void must(int condition, const char *message)
{
   if (!condition)
   {
      fprintf(stderr, "FAIL: %s\n", message);
      abort();
   }
}

cJSON *config_client_transport_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                                    size_t max_body, int timeout_ms,
                                    ouro_module_call_result_t *result)
{
   (void)event_kind, (void)stage_id, (void)max_body, (void)timeout_ms;
   *result = OURO_MODULE_CALL_OK;
   cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation");
   must(cJSON_IsString(operation), "request has an operation");
   cJSON *response = cJSON_CreateObject();
   cJSON_AddTrueToObject(response, "ok");
   if (!strcmp(operation->valuestring, "snapshot"))
   {
      snapshot_calls++;
      if (during_snapshot)
         during_snapshot();
      cJSON *values = cJSON_AddObjectToObject(response, "values");
      cJSON_AddNumberToObject(values, "max_iterations", 37);
      cJSON_AddStringToObject(response, "version", current_version);
   }
   else if (!strcmp(operation->valuestring, "version"))
   {
      version_calls++;
      cJSON_AddStringToObject(response, "version", current_version);
   }
   cJSON_Delete(request);
   return response;
}

static void announce(const char *version)
{
   char payload[96];
   int n = snprintf(payload, sizeof payload, "{\"version\":\"%s\"}", version);
   config_client_note_announcement((const uint8_t *)payload, (uint32_t)n);
}

static void announce_b(void)
{
   announce(VERSION_B);
}

int main(void)
{
   printf("test_config_announcements:\n");
   config_client_use_announcements();

   must(config_client_changed() == 1, "nothing fetched yet is a change");
   must(config_client_refresh() == 0 && snapshot_calls == 1, "first snapshot");
   for (int i = 0; i < 100; i++)
      must(config_client_changed() == 0, "unchanged while nothing is announced");
   must(version_calls == 0, "and the module is never asked for its version");

   announce(VERSION_A);
   must(config_client_changed() == 0, "an echo of the held version is not a change");

   current_version = VERSION_B;
   announce(VERSION_B);
   must(config_client_changed() == 1, "a new version is a change");
   must(config_client_refresh() == 0 && snapshot_calls == 2, "refreshed once");
   must(config_client_changed() == 0, "and is current after the refresh");

   config_client_note_announcement(NULL, 0);
   must(config_client_changed() == 1, "a missed announcement means re-read");
   must(config_client_refresh() == 0, "refresh after a missed announcement");
   must(config_client_changed() == 0, "current again");

   /* An announcement that lands while a snapshot is in flight is newer than
    * what the snapshot returns, so the client must look again. */
   current_version = VERSION_A;
   during_snapshot = announce_b;
   must(config_client_refresh() == 0, "a snapshot that raced an announcement");
   during_snapshot = NULL;
   must(config_client_changed() == 1, "the raced announcement still counts");

   must(version_calls == 0, "no version call at any point");
   printf("test_config_announcements: OK\n");
   return 0;
}
