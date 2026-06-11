/* test_anthropic_shape.c: unit tests for §3 Anthropic system cache-shaping. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h" /* MAX_PATH_LEN, pulled in before agent_types.h */
#include "agent_protocol.h"
#include "cJSON.h"

#define PASS(name) printf("  %s: ok\n", name)

static void test_plain_string_when_unmarked(void)
{
   cJSON *req = cJSON_CreateObject();
   agent_anthropic_set_system(req, "You are aimee.", 0);
   cJSON *sys = cJSON_GetObjectItem(req, "system");
   /* Unmarked -> a plain string, no cache_control anywhere. */
   assert(cJSON_IsString(sys));
   assert(strcmp(sys->valuestring, "You are aimee.") == 0);
   char *txt = cJSON_PrintUnformatted(req);
   assert(strstr(txt, "cache_control") == NULL);
   free(txt);
   cJSON_Delete(req);
   PASS("shape: plain string when cache marking off");
}

static void test_cached_block_when_marked(void)
{
   cJSON *req = cJSON_CreateObject();
   agent_anthropic_set_system(req, "You are aimee.", 1);
   cJSON *sys = cJSON_GetObjectItem(req, "system");
   /* Marked -> a content-block array carrying cache_control: ephemeral. */
   assert(cJSON_IsArray(sys));
   assert(cJSON_GetArraySize(sys) == 1);
   cJSON *block = cJSON_GetArrayItem(sys, 0);
   assert(strcmp(cJSON_GetObjectItem(block, "type")->valuestring, "text") == 0);
   assert(strcmp(cJSON_GetObjectItem(block, "text")->valuestring, "You are aimee.") == 0);
   cJSON *cc = cJSON_GetObjectItem(block, "cache_control");
   assert(cJSON_IsObject(cc));
   assert(strcmp(cJSON_GetObjectItem(cc, "type")->valuestring, "ephemeral") == 0);
   cJSON_Delete(req);
   PASS("shape: cached content block when marking on");
}

static void test_empty_system_adds_nothing(void)
{
   cJSON *req = cJSON_CreateObject();
   agent_anthropic_set_system(req, "", 1);
   assert(cJSON_GetObjectItem(req, "system") == NULL);
   agent_anthropic_set_system(req, NULL, 1);
   assert(cJSON_GetObjectItem(req, "system") == NULL);
   /* NULL req must not crash. */
   agent_anthropic_set_system(NULL, "x", 1);
   cJSON_Delete(req);
   PASS("shape: empty/NULL system adds nothing");
}

int main(void)
{
   printf("anthropic_shape: unit tests\n");
   test_plain_string_when_unmarked();
   test_cached_block_when_marked();
   test_empty_system_adds_nothing();
   printf("All anthropic_shape tests passed.\n");
   return 0;
}
