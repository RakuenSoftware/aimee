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
   agent_anthropic_set_system(req, "You are aimee.", 0, 0);
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
   agent_anthropic_set_system(req, "You are aimee.", 1, 0);
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
   agent_anthropic_set_system(req, "", 1, 0);
   assert(cJSON_GetObjectItem(req, "system") == NULL);
   agent_anthropic_set_system(req, NULL, 1, 0);
   assert(cJSON_GetObjectItem(req, "system") == NULL);
   /* NULL req must not crash. */
   agent_anthropic_set_system(NULL, "x", 1, 0);
   cJSON_Delete(req);
   PASS("shape: empty/NULL system adds nothing");
}

static int json_has_substr(cJSON *node, const char *needle)
{
   char *s = cJSON_PrintUnformatted(node);
   int found = s && strstr(s, needle) != NULL;
   free(s);
   return found;
}

static void test_volatile_context_not_cached(void)
{
   /* A system that is ENTIRELY the per-turn <aimee-context> envelope must NOT be
    * marked cacheable (no cache_control), or the cache is busted every turn. */
   const char *vol = "<aimee-context confidence=\"high\">recent files: foo.c</aimee-context>";
   cJSON *req = cJSON_CreateObject();
   agent_anthropic_set_system(req, vol, 1, 0);
   cJSON *sys = cJSON_GetObjectItem(req, "system");
   assert(cJSON_IsString(sys)); /* plain string, not a cache-marked block */
   assert(!json_has_substr(sys, "cache_control"));
   cJSON_Delete(req);
   PASS("shape: volatile <aimee-context> is not cache-marked");
}

static void test_stable_prefix_cached_volatile_not(void)
{
   /* A stable persona prefix followed by the volatile <aimee-context> envelope:
    * cache ONLY the stable prefix; leave the context uncached. */
   const char *mixed = "You are aimee, a helpful agent.\n<aimee-context confidence=\"high\">files: "
                       "x</aimee-context>";
   cJSON *req = cJSON_CreateObject();
   agent_anthropic_set_system(req, mixed, 1, 0);
   cJSON *sys = cJSON_GetObjectItem(req, "system");
   assert(cJSON_IsArray(sys));
   assert(cJSON_GetArraySize(sys) == 2);

   cJSON *stable = cJSON_GetArrayItem(sys, 0);
   cJSON *vol = cJSON_GetArrayItem(sys, 1);
   /* Stable prefix: cached, and does NOT contain the volatile marker. */
   assert(cJSON_IsObject(cJSON_GetObjectItem(stable, "cache_control")));
   assert(strstr(cJSON_GetObjectItem(stable, "text")->valuestring, "You are aimee") != NULL);
   assert(strstr(cJSON_GetObjectItem(stable, "text")->valuestring, "<aimee-context") == NULL);
   /* Volatile block: holds the context, and is NOT cache-marked. */
   assert(cJSON_GetObjectItem(vol, "cache_control") == NULL);
   assert(strstr(cJSON_GetObjectItem(vol, "text")->valuestring, "<aimee-context") != NULL);
   cJSON_Delete(req);
   PASS("shape: stable prefix cached, volatile context not");
}

static void test_min_chars_applies_to_stable_prefix(void)
{
   /* A tiny stable prefix in front of a LARGE volatile <aimee-context> must NOT be
    * cache-marked when the prefix is below the min_chars floor — the floor checks
    * the cacheable stable prefix, not the whole prompt. */
   const char *mixed = "Hi.\n<aimee-context confidence=\"high\">"
                       "a very large query-derived block of context that dwarfs the prefix..."
                       "</aimee-context>";
   cJSON *req = cJSON_CreateObject();
   /* Prefix "Hi.\n" is ~4 chars; floor 32 -> not cached despite the long whole. */
   agent_anthropic_set_system(req, mixed, 1, 32);
   cJSON *sys = cJSON_GetObjectItem(req, "system");
   assert(cJSON_IsString(sys));
   assert(!json_has_substr(sys, "cache_control"));
   cJSON_Delete(req);

   /* The same prefix with a low floor does get cached (and split). */
   req = cJSON_CreateObject();
   agent_anthropic_set_system(req, mixed, 1, 2);
   sys = cJSON_GetObjectItem(req, "system");
   assert(cJSON_IsArray(sys));
   assert(json_has_substr(cJSON_GetArrayItem(sys, 0), "cache_control"));
   cJSON_Delete(req);
   PASS("shape: min_chars floors on the stable prefix, not the whole prompt");
}

int main(void)
{
   printf("anthropic_shape: unit tests\n");
   test_plain_string_when_unmarked();
   test_cached_block_when_marked();
   test_empty_system_adds_nothing();
   test_volatile_context_not_cached();
   test_stable_prefix_cached_volatile_not();
   test_min_chars_applies_to_stable_prefix();
   printf("All anthropic_shape tests passed.\n");
   return 0;
}
