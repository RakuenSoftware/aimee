#include "aimee.h"
#include "cJSON.h"
#include "modules/db2/c/memory_query.h"
#include <assert.h>
#include <string.h>

extern cJSON *db2_kb_service_memory_list_json(const char *, const char *, int);
static int result;
int memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   (void)tier;
   (void)kind;
   (void)limit;
   (void)out;
   (void)max;
   return result;
}
int db2_memory_summaries_list(int64_t id, int limit, db2_memory_summary_row_t *out, int max)
{
   (void)id;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}
int main(void)
{
   result = -1;
   cJSON *reply = db2_kb_service_memory_list_json(NULL, NULL, 20);
   assert(strcmp(cJSON_GetObjectItem(reply, "status")->valuestring, "error") == 0);
   assert(strcmp(cJSON_GetObjectItem(reply, "kind")->valuestring, "unavailable") == 0);
   cJSON_Delete(reply);
   result = 0;
   reply = db2_kb_service_memory_list_json(NULL, NULL, 20);
   assert(strcmp(cJSON_GetObjectItem(reply, "status")->valuestring, "ok") == 0);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply, "memories")) == 0);
   cJSON_Delete(reply);
   return 0;
}
