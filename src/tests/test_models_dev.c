/* test_models_dev.c: tests for models.dev cache lookup */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "models_dev.h"

static void test_cache_lookup_hit(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/test-models-dev-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cache_parent[512], cache_dir[512], cache_path[512];
   snprintf(cache_parent, sizeof(cache_parent), "%s/.cache", tmpdir);
   snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/aimee", tmpdir);
   snprintf(cache_path, sizeof(cache_path), "%s/models_dev.json", cache_dir);
   mkdir(cache_parent, 0755);
   mkdir(cache_dir, 0755);

   const char *json =
       "{\"anthropic/claude-test\": {\"contextWindow\": 200000, \"maxTokens\": 4096,"
       " \"inputCost\": 3.0, \"outputCost\": 15.0, \"tools\": true, \"vision\": false}}";
   FILE *f = fopen(cache_path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);

   setenv("HOME", tmpdir, 1);

   model_capability_t caps;
   memset(&caps, 0, sizeof(caps));
   int rc = models_dev_cache_lookup("anthropic", "claude-test", &caps);
   assert(rc == 1);
   assert(caps.context_window == 200000);
   assert(caps.max_output == 4096);
   assert(caps.cost_in_per_mtok == 3.0);
   assert(caps.flags & MODEL_CAP_TOOLS);
   assert(!(caps.flags & MODEL_CAP_VISION));

   unlink(cache_path);
   rmdir(cache_dir);
   rmdir(cache_parent);
   rmdir(tmpdir);
}

/* The LIVE https://models.dev/api.json schema is NESTED, and models_dev_refresh()
 * curls it into the cache verbatim with no transform. Before the reader learned
 * this shape the downloaded cache resolved NOTHING — the flat "provider/model"
 * key lookup returned NULL against a nested root — so every capability fell
 * through to the heuristic and every price stayed 0. Uses the real field names
 * and real values for two live fleet models. */
static void test_cache_lookup_nested_api_schema(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof(tmpdir), "/tmp/test-models-dev-nested-XXXXXX");
   assert(mkdtemp(tmpdir) != NULL);

   char cache_parent[512], cache_dir[512], cache_path[512];
   snprintf(cache_parent, sizeof(cache_parent), "%s/.cache", tmpdir);
   snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/aimee", tmpdir);
   snprintf(cache_path, sizeof(cache_path), "%s/models_dev.json", cache_dir);
   mkdir(cache_parent, 0755);
   mkdir(cache_dir, 0755);

   const char *json =
       "{\"minimax\": {\"name\": \"MiniMax\", \"models\": {"
       "  \"MiniMax-M3\": {\"name\": \"MiniMax M3\","
       "    \"limit\": {\"context\": 1000000, \"output\": 128000},"
       "    \"cost\": {\"input\": 0.3, \"output\": 1.2},"
       "    \"tool_call\": true, \"reasoning\": true,"
       "    \"modalities\": {\"input\": [\"text\", \"image\"]}}}},"
       " \"openai\": {\"models\": {"
       "  \"gpt-5.6-sol\": {\"name\": \"GPT-5.6 Sol\","
       "    \"limit\": {\"context\": 1050000, \"output\": 128000},"
       "    \"cost\": {\"input\": 5.0, \"output\": 30.0},"
       "    \"tool_call\": true, \"reasoning\": true}}}}";
   FILE *f = fopen(cache_path, "w");
   assert(f);
   fputs(json, f);
   fclose(f);

   setenv("HOME", tmpdir, 1);

   model_capability_t caps;
   memset(&caps, 0, sizeof(caps));
   assert(models_dev_cache_lookup("minimax", "MiniMax-M3", &caps) == 1);
   assert(caps.context_window == 1000000);
   assert(caps.max_output == 128000);
   assert(caps.cost_in_per_mtok == 0.3);
   assert(caps.cost_out_per_mtok == 1.2);
   assert(caps.flags & MODEL_CAP_TOOLS);
   /* REASONING has no flat-schema equivalent; the nested reader is the only
    * source of it, and it is what selects the long per-call timeout. */
   assert(caps.flags & MODEL_CAP_REASONING);
   assert(caps.flags & MODEL_CAP_VISION);
   assert(strcmp(caps.display_name, "MiniMax M3") == 0);
   assert(!caps.deprecated);

   /* Price is the whole point of reading this schema: it is the only source of
    * a real cost basis, and it is 0 for every model without it. */
   memset(&caps, 0, sizeof(caps));
   assert(models_dev_cache_lookup("openai", "gpt-5.6-sol", &caps) == 1);
   assert(caps.cost_in_per_mtok == 5.0);
   assert(caps.cost_out_per_mtok == 30.0);
   assert(strcmp(caps.display_name, "GPT-5.6 Sol") == 0);

   /* A provider present but a model absent must still miss cleanly. */
   memset(&caps, 0, sizeof(caps));
   assert(models_dev_cache_lookup("minimax", "MiniMax-M99", &caps) == 0);

   unlink(cache_path);
   rmdir(cache_dir);
   rmdir(cache_parent);
   rmdir(tmpdir);
}

static void test_cache_lookup_miss(void)
{
   model_capability_t caps;
   memset(&caps, 0, sizeof(caps));
   int rc = models_dev_cache_lookup("unknown", "nonexistent-model", &caps);
   assert(rc == 0);
}

static void test_cache_lookup_null_guard(void)
{
   assert(models_dev_cache_lookup(NULL, "model", NULL) == 0);
   assert(models_dev_cache_lookup("prov", NULL, NULL) == 0);
}

static void test_stub_returns_zero(void)
{
   model_capability_t caps;
   int rc = models_dev_capability_get("anthropic", "claude-opus-4-6", &caps);
   assert(rc == 0);
}

int main(void)
{
   printf("models_dev: ");
   test_cache_lookup_hit();
   printf("cache_hit OK, ");
   test_cache_lookup_nested_api_schema();
   printf("nested_api OK, ");
   test_cache_lookup_miss();
   printf("cache_miss OK, ");
   test_cache_lookup_null_guard();
   printf("null_guard OK, ");
   test_stub_returns_zero();
   printf("stub OK\n");
   return 0;
}
