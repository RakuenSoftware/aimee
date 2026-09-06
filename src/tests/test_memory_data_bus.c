#include "aimee.h"
#include "cJSON.h"
#include "headers/module_json_call.h"
#include <aimee/memory/module_api.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char *content;
static int mode;
cJSON *aimee_module_json_call(uint32_t event, uint32_t stage, cJSON *request, size_t max_body,
                              int timeout, aimee_module_call_result_t *result)
{
   assert(event == AIMEE_MEMORY_EVENT_DATA && stage == AIMEE_MEMORY_STAGE_DATA);
   assert(strcmp(cJSON_GetObjectItem(request, "operation")->valuestring, "get") == 0);
   cJSON_Delete(request);
   (void)max_body;
   (void)timeout;
   *result = mode < 0 ? AIMEE_MODULE_CALL_INTERNAL : AIMEE_MODULE_CALL_OK;
   if (mode < 0)
      return NULL;
   cJSON *reply = cJSON_CreateObject();
   cJSON *records = cJSON_AddArrayToObject(reply, "records");
   if (mode == 1)
      return reply;
   cJSON *row = cJSON_Parse(
       "{\"id\":42,\"tier\":\"L0\",\"kind\":\"fact\",\"key\":\"fixture\",\"confidence\":1}");
   cJSON_AddStringToObject(row, "content", content);
   cJSON_AddItemToArray(records, row);
   return reply;
}

int main(void)
{
   char *long_text = malloc(16001);
   assert(long_text);
   for (int i = 0; i < 4000; ++i)
      memcpy(long_text + i * 4, "🦊", 4);
   long_text[16000] = 0;
   content = long_text;
   memory_t row;
   assert(memory_get(42, &row) == 0); /* Long records remain valid through the fixed ABI. */
   assert(strlen(row.content) > 0 && strlen(row.content) < sizeof(row.content));
   assert(strlen(row.content) % 4 == 0); /* Preview does not cut a Unicode code point. */
   char *full = memory_content_dup(42);
   assert(full && strcmp(full, long_text) == 0);
   free(full);
   mode = 1;
   assert(memory_get_result(42, &row) == 1);
   assert(memory_get(42, &row) == -1); /* Existing ABI unchanged. */
   mode = -1;
   assert(memory_get_result(42, &row) == -1);
   free(long_text);
   return 0;
}
